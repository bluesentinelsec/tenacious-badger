#define WIN32_LEAN_AND_MEAN
#include "../shared/fs.h"
#include "badger/assert.h"
#include <limits.h>
#include <shlobj.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

typedef struct tb_native_directory
{
    HANDLE handle;
    WIN32_FIND_DATAW entry;
    bool pending;
    bool ended;
} tb_native_directory;

static tb_fs_result tb_fs_windows_error(DWORD code)
{
    switch (code)
    {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return TB_FS_NOT_FOUND;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return TB_FS_EXISTS;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_WRITE_PROTECT:
        return TB_FS_ACCESS;
    case ERROR_DIRECTORY:
        return TB_FS_NOT_DIRECTORY;
    case ERROR_DIR_NOT_EMPTY:
        return TB_FS_NOT_EMPTY;
    case ERROR_INVALID_NAME:
    case ERROR_INVALID_PARAMETER:
    case ERROR_NEGATIVE_SEEK:
    case ERROR_NO_UNICODE_TRANSLATION:
        return TB_FS_INVALID;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
    case ERROR_DISK_QUOTA_EXCEEDED:
        return TB_FS_NO_SPACE;
    case ERROR_INSUFFICIENT_BUFFER:
        return TB_FS_BUFFER_TOO_SMALL;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
    case ERROR_TOO_MANY_OPEN_FILES:
        return TB_FS_NO_MEMORY;
    case ERROR_FILENAME_EXCED_RANGE:
    case ERROR_FILE_TOO_LARGE:
    case ERROR_ARITHMETIC_OVERFLOW:
        return TB_FS_TOO_LARGE;
    default:
        return TB_FS_OS_ERROR;
    }
}
static tb_fs_result tb_fs_last_error(void)
{
    return tb_fs_windows_error(GetLastError());
}

/* Prefixing an ordinary path must not silently turn a DOS device name into
 * a disk filename. Reject ambiguous components before enabling long paths. */
static bool tb_fs_windows_components(const WCHAR *name, size_t length)
{
    size_t start = {0};
    for (size_t end = {0}; end <= length; end++)
    {
        if (end != length && name[end] != L'\\')
        {
            if (name[end] < 32 || name[end] == L'"' || name[end] == L'<' || name[end] == L'>' ||
                name[end] == L'|' || name[end] == L'*')
            {
                return false;
            }
            if (name[end] == L'?' && end != 2)
            {
                return false;
            }
            if (name[end] == L':' && !(end == 1 || (end == 5 && name[2] == L'?')))
            {
                return false;
            }
            continue;
        }
        if (end > start)
        {
            WCHAR stem[5] = {0};
            size_t count = {0};
            bool dot = {0};
            dot = end - start == 1 && name[start] == L'.';
            dot = dot || (end - start == 2 && name[start] == L'.' && name[start + 1] == L'.');
            if (!dot && (name[end - 1] == L'.' || name[end - 1] == L' '))
            {
                return false;
            }
            while (count < 4 && start + count < end && name[start + count] != L'.')
            {
                WCHAR byte = {0};
                byte = name[start + count];
                if (byte >= L'a' && byte <= L'z')
                {
                    byte = (WCHAR)(byte - L'a' + L'A');
                }
                stem[count] = byte;
                count++;
            }
            if ((start + count == end || name[start + count] == L'.') &&
                ((count == 3 && (memcmp(stem, L"CON", 3 * sizeof(WCHAR)) == 0 ||
                                 memcmp(stem, L"PRN", 3 * sizeof(WCHAR)) == 0 ||
                                 memcmp(stem, L"AUX", 3 * sizeof(WCHAR)) == 0 ||
                                 memcmp(stem, L"NUL", 3 * sizeof(WCHAR)) == 0)) ||
                 (count == 4 &&
                  (memcmp(stem, L"COM", 3 * sizeof(WCHAR)) == 0 ||
                   memcmp(stem, L"LPT", 3 * sizeof(WCHAR)) == 0) &&
                  ((stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == 0xB9 || stem[3] == 0xB2 ||
                   stem[3] == 0xB3))))
            {
                return false;
            }
        }
        start = end + 1;
    }
    return true;
}
static tb_fs_result tb_fs_windows_path(tb_path_view path, WCHAR **output)
{
    WCHAR *wide = NULL;
    WCHAR *absolute = NULL;
    WCHAR *prefixed = NULL;
    int units = {0};
    DWORD needed = {0};
    DWORD written = {0};
    size_t prefix = {0};
    size_t skip = {0};
    tb_fs_result result = {0};
    *output = NULL;
    if (path.length > 131072)
    {
        return TB_FS_TOO_LARGE;
    }
    if (path.length == 0 || !tb_native_fs_text_valid(path.data, path.length))
    {
        return TB_FS_INVALID;
    }
    units =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data, (int)path.length, NULL, 0);
    if (units == 0)
    {
        return tb_fs_last_error();
    }
    if (units >= 32760)
    {
        return TB_FS_TOO_LARGE;
    }
    wide = malloc(((size_t)units + 1) * sizeof(WCHAR));
    if (wide == NULL)
    {
        return TB_FS_NO_MEMORY;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data, (int)path.length, wide,
                            units) == 0)
    {
        result = tb_fs_last_error();
        goto cleanup;
    }
    wide[units] = L'\0';
    for (size_t index = {0}; index < (size_t)units; index++)
    {
        if (wide[index] == L'/')
        {
            wide[index] = L'\\';
        }
    }
    if (!tb_fs_windows_components(wide, (size_t)units))
    {
        result = TB_FS_INVALID;
        goto cleanup;
    }
    if (units >= 4 && wide[0] == L'\\' && wide[1] == L'\\' &&
        (wide[2] == L'.' || wide[2] == L'?') && wide[3] == L'\\')
    {
        bool drive = {0};
        bool unc = {0};
        drive = units >= 7 && wide[5] == L':' && wide[6] == L'\\' &&
                ((wide[4] >= L'A' && wide[4] <= L'Z') || (wide[4] >= L'a' && wide[4] <= L'z'));
        unc = units > 8 && memcmp(wide + 4, L"UNC\\", 4 * sizeof(WCHAR)) == 0;
        if (wide[2] != L'?' || (!drive && !unc))
        {
            result = TB_FS_INVALID;
            goto cleanup;
        }
        *output = wide;
        wide = NULL;
        goto cleanup;
    }
    needed = GetFullPathNameW(wide, 0, NULL, NULL);
    if (needed == 0)
    {
        result = tb_fs_last_error();
        goto cleanup;
    }
    if (needed >= 32760)
    {
        result = TB_FS_TOO_LARGE;
        goto cleanup;
    }
    absolute = malloc((size_t)needed * sizeof(WCHAR));
    if (absolute == NULL)
    {
        result = TB_FS_NO_MEMORY;
        goto cleanup;
    }
    written = GetFullPathNameW(wide, needed, absolute, NULL);
    if (written == 0)
    {
        result = tb_fs_last_error();
        goto cleanup;
    }
    if (written >= needed)
    {
        result = TB_FS_TOO_LARGE;
        goto cleanup;
    }
    prefix = 4;
    if (written >= 2 && absolute[0] == L'\\' && absolute[1] == L'\\')
    {
        prefix = 8;
        skip = 2;
    }
    prefixed = malloc(((size_t)written + prefix + 1) * sizeof(WCHAR));
    if (prefixed == NULL)
    {
        result = TB_FS_NO_MEMORY;
        goto cleanup;
    }
    memcpy(prefixed, prefix == 8 ? L"\\\\?\\UNC\\" : L"\\\\?\\", prefix * sizeof(WCHAR));
    memcpy(prefixed + prefix, absolute + skip, ((size_t)written - skip + 1) * sizeof(WCHAR));
    *output = prefixed;
    prefixed = NULL;
cleanup:
    free(wide);
    free(absolute);
    free(prefixed);
    return result;
}
static tb_fs_result tb_fs_windows_utf8(const WCHAR *wide, char **text)
{
    int needed = {0};
    *text = NULL;
    needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, NULL, 0, NULL, NULL);
    if (needed == 0)
    {
        return tb_fs_last_error();
    }
    *text = malloc((size_t)needed);
    if (*text == NULL)
    {
        return TB_FS_NO_MEMORY;
    }
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, *text, needed, NULL, NULL) ==
        0)
    {
        tb_fs_result result = {0};
        result = tb_fs_last_error();
        free(*text);
        *text = NULL;
        return result;
    }
    for (size_t index = {0}; (*text)[index] != '\0'; index++)
    {
        if ((*text)[index] == '\\')
        {
            (*text)[index] = '/';
        }
    }
    return TB_FS_OK;
}
static HANDLE tb_file_handle(const tb_file *file)
{
    TB_ASSERT(file != NULL && file->value != 0);
    return (HANDLE)file->value;
}
tb_fs_result tb_file_open(tb_path_view path, unsigned flags, tb_file *file)
{
    WCHAR *name = NULL;
    HANDLE handle = NULL;
    DWORD access = {0};
    DWORD creation = {0};
    BY_HANDLE_FILE_INFORMATION metadata = {0};
    tb_fs_result result = {0};
    TB_ASSERT(file != NULL && file->value == 0);
    if ((flags & ~(unsigned)(TB_FILE_READ | TB_FILE_WRITE | TB_FILE_CREATE | TB_FILE_EXCLUSIVE |
                             TB_FILE_TRUNCATE)) != 0 ||
        (flags & (TB_FILE_READ | TB_FILE_WRITE)) == 0 ||
        ((flags & (TB_FILE_CREATE | TB_FILE_TRUNCATE)) != 0 && (flags & TB_FILE_WRITE) == 0) ||
        ((flags & TB_FILE_EXCLUSIVE) != 0 && (flags & TB_FILE_CREATE) == 0))
    {
        return TB_FS_INVALID;
    }
    result = tb_fs_windows_path(path, &name);
    if (result != TB_FS_OK)
    {
        return result;
    }
    if ((flags & TB_FILE_READ) != 0)
    {
        access |= GENERIC_READ;
    }
    if ((flags & TB_FILE_WRITE) != 0)
    {
        access |= GENERIC_WRITE;
    }
    creation = OPEN_EXISTING;
    if ((flags & TB_FILE_CREATE) != 0)
    {
        creation = (flags & TB_FILE_EXCLUSIVE) != 0 ? CREATE_NEW : OPEN_ALWAYS;
    }
    handle = CreateFileW(name, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                         creation, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(name);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return tb_fs_last_error();
    }
    if (!GetFileInformationByHandle(handle, &metadata))
    {
        result = tb_fs_last_error();
        goto failed;
    }
    if ((metadata.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        result = TB_FS_IS_DIRECTORY;
        goto failed;
    }
    if (GetFileType(handle) != FILE_TYPE_DISK)
    {
        result = TB_FS_INVALID;
        goto failed;
    }
    if ((flags & TB_FILE_TRUNCATE) != 0 && !SetEndOfFile(handle))
    {
        result = tb_fs_last_error();
        goto failed;
    }
    file->value = (uintptr_t)handle;
    return TB_FS_OK;
failed:
    (void)CloseHandle(handle);
    return result;
}
tb_fs_result tb_file_read(tb_file *file, void *bytes, size_t capacity, size_t *transferred)
{
    DWORD count = {0};
    HANDLE handle = NULL;
    TB_ASSERT(transferred != NULL);
    TB_ASSERT(bytes != NULL || capacity == 0);
    *transferred = 0;
    handle = tb_file_handle(file);
    if (capacity == 0)
    {
        return TB_FS_OK;
    }
    if (capacity > INT_MAX)
    {
        capacity = INT_MAX;
    }
    if (!ReadFile(handle, bytes, (DWORD)capacity, &count, NULL))
    {
        return tb_fs_last_error();
    }
    *transferred = count;
    return TB_FS_OK;
}
tb_fs_result tb_file_write(tb_file *file, const void *bytes, size_t length, size_t *transferred)
{
    DWORD count = {0};
    HANDLE handle = NULL;
    TB_ASSERT(transferred != NULL);
    TB_ASSERT(bytes != NULL || length == 0);
    *transferred = 0;
    handle = tb_file_handle(file);
    if (length == 0)
    {
        return TB_FS_OK;
    }
    if (length > INT_MAX)
    {
        length = INT_MAX;
    }
    if (!WriteFile(handle, bytes, (DWORD)length, &count, NULL))
    {
        return tb_fs_last_error();
    }
    *transferred = count;
    return TB_FS_OK;
}
tb_fs_result tb_file_seek(tb_file *file, int64_t offset, tb_file_origin origin, uint64_t *position)
{
    DWORD method = {0};
    LARGE_INTEGER distance = {0};
    LARGE_INTEGER answer = {0};
    TB_ASSERT(position != NULL);
    switch (origin)
    {
    case TB_FILE_START:
        method = FILE_BEGIN;
        break;
    case TB_FILE_CURRENT:
        method = FILE_CURRENT;
        break;
    case TB_FILE_END:
        method = FILE_END;
        break;
    default:
        return TB_FS_INVALID;
    }
    distance.QuadPart = offset;
    if (!SetFilePointerEx(tb_file_handle(file), distance, &answer, method))
    {
        return tb_fs_last_error();
    }
    *position = (uint64_t)answer.QuadPart;
    return TB_FS_OK;
}
tb_fs_result tb_file_tell(tb_file *file, uint64_t *position)
{
    return tb_file_seek(file, 0, TB_FILE_CURRENT, position);
}
tb_fs_result tb_file_size(tb_file *file, uint64_t *size)
{
    LARGE_INTEGER answer = {0};
    TB_ASSERT(size != NULL);
    if (!GetFileSizeEx(tb_file_handle(file), &answer))
    {
        return tb_fs_last_error();
    }
    *size = (uint64_t)answer.QuadPart;
    return TB_FS_OK;
}
tb_fs_result tb_file_flush(tb_file *file)
{
    return FlushFileBuffers(tb_file_handle(file)) ? TB_FS_OK : tb_fs_last_error();
}
tb_fs_result tb_file_close(tb_file *file)
{
    HANDLE handle = NULL;
    TB_ASSERT(file != NULL);
    if (file->value == 0)
    {
        return TB_FS_OK;
    }
    handle = tb_file_handle(file);
    file->value = 0;
    return CloseHandle(handle) ? TB_FS_OK : tb_fs_last_error();
}
tb_fs_result tb_fs_query(tb_path_view path, tb_fs_info *info)
{
    WCHAR *name = NULL;
    WIN32_FILE_ATTRIBUTE_DATA data = {0};
    tb_fs_info found = {0};
    uint64_t ticks = {0};
    tb_fs_result result = {0};
    TB_ASSERT(info != NULL);
    result = tb_fs_windows_path(path, &name);
    if (result != TB_FS_OK)
    {
        return result;
    }
    if (!GetFileAttributesExW(name, GetFileExInfoStandard, &data))
    {
        result = tb_fs_last_error();
        goto cleanup;
    }
    found.kind = TB_FS_REGULAR;
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        found.kind = TB_FS_DIRECTORY;
    }
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        found.kind = TB_FS_LINK;
    }
    found.size = ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    ticks =
        ((uint64_t)data.ftLastWriteTime.dwHighDateTime << 32) | data.ftLastWriteTime.dwLowDateTime;
    found.modified_seconds = (int64_t)(ticks / UINT64_C(10000000)) - INT64_C(11644473600);
    found.modified_nanoseconds = (uint32_t)(ticks % UINT64_C(10000000)) * 100;
    *info = found;
cleanup:
    free(name);
    return result;
}
tb_fs_result tb_fs_make_directory(tb_path_view path)
{
    WCHAR *name = NULL;
    tb_fs_result result = {0};
    result = tb_fs_windows_path(path, &name);
    if (result != TB_FS_OK)
    {
        return result;
    }
    if (!CreateDirectoryW(name, NULL))
    {
        result = tb_fs_last_error();
    }
    free(name);
    return result;
}
tb_fs_result tb_fs_remove(tb_path_view path)
{
    WCHAR *name = NULL;
    DWORD attributes = {0};
    BOOL removed = {0};
    tb_fs_result result = {0};
    result = tb_fs_windows_path(path, &name);
    if (result != TB_FS_OK)
    {
        return result;
    }
    attributes = GetFileAttributesW(name);
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        result = tb_fs_last_error();
        goto cleanup;
    }
    removed =
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? RemoveDirectoryW(name) : DeleteFileW(name);
    if (!removed)
    {
        result = tb_fs_last_error();
    }
cleanup:
    free(name);
    return result;
}
tb_fs_result tb_fs_rename(tb_path_view source, tb_path_view destination)
{
    WCHAR *from = NULL;
    WCHAR *to = NULL;
    HANDLE handle = INVALID_HANDLE_VALUE;
    FILE_RENAME_INFO *information = NULL;
    size_t length = {0};
    size_t allocation = {0};
    DWORD failure = {0};
    tb_fs_result result = {0};
    result = tb_fs_windows_path(source, &from);
    if (result != TB_FS_OK)
    {
        goto cleanup;
    }
    result = tb_fs_windows_path(destination, &to);
    if (result != TB_FS_OK)
    {
        goto cleanup;
    }
    while (to[length] != L'\0')
    {
        length++;
    }
    /* The path converter limits this to fewer than 32768 UTF-16 units. */
    allocation = sizeof(*information) + length * sizeof(WCHAR);
    information = malloc(allocation);
    if (information == NULL)
    {
        result = TB_FS_NO_MEMORY;
        goto cleanup;
    }
    *information = (FILE_RENAME_INFO){0};
    information->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    information->FileNameLength = (DWORD)(length * sizeof(WCHAR));
    memcpy(information->FileName, to, (length + 1) * sizeof(WCHAR));
    handle =
        CreateFileW(from, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (handle == INVALID_HANDLE_VALUE)
    {
        result = tb_fs_last_error();
        goto cleanup;
    }
    if (!SetFileInformationByHandle(handle, FileRenameInfoEx, information, (DWORD)allocation))
    {
        failure = GetLastError();
        result = tb_fs_windows_error(failure);
        /* Older/network filesystems can reject the extended information class.
         * Their traditional rename may require all destination readers closed. */
        if (failure == ERROR_INVALID_PARAMETER || failure == ERROR_NOT_SUPPORTED ||
            failure == ERROR_INVALID_FUNCTION)
        {
            (void)CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
            result = MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
                         ? TB_FS_OK
                         : tb_fs_last_error();
        }
    }
cleanup:
    if (handle != INVALID_HANDLE_VALUE)
    {
        (void)CloseHandle(handle);
    }
    free(information);
    free(from);
    free(to);
    return result;
}
tb_fs_result tb_directory_open(tb_path_view path, tb_directory *directory)
{
    WCHAR *name = NULL;
    WCHAR *pattern = NULL;
    tb_native_directory *state = NULL;
    DWORD attributes = {0};
    size_t length = {0};
    tb_fs_result result = {0};
    TB_ASSERT(directory != NULL && directory->state == NULL);
    result = tb_fs_windows_path(path, &name);
    if (result != TB_FS_OK)
    {
        goto cleanup;
    }
    attributes = GetFileAttributesW(name);
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        result = tb_fs_last_error();
        goto cleanup;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        result = TB_FS_NOT_DIRECTORY;
        goto cleanup;
    }
    while (name[length] != L'\0')
    {
        length++;
    }
    pattern = malloc((length + 3) * sizeof(WCHAR));
    state = malloc(sizeof(*state));
    if (pattern == NULL || state == NULL)
    {
        result = TB_FS_NO_MEMORY;
        goto cleanup;
    }
    *state = (tb_native_directory){0};
    memcpy(pattern, name, length * sizeof(WCHAR));
    pattern[length] = L'\\';
    pattern[length + 1] = L'*';
    pattern[length + 2] = L'\0';
    state->handle = FindFirstFileW(pattern, &state->entry);
    if (state->handle == INVALID_HANDLE_VALUE)
    {
        if (GetLastError() != ERROR_FILE_NOT_FOUND)
        {
            result = tb_fs_last_error();
            goto cleanup;
        }
        state->ended = true;
    }
    else
    {
        state->pending = true;
    }
    directory->state = state;
    state = NULL;
cleanup:
    free(state);
    free(pattern);
    free(name);
    return result;
}
tb_fs_result tb_directory_next(tb_directory *directory, char *name, size_t capacity,
                               size_t *required)
{
    tb_native_directory *state = NULL;
    char *text = NULL;
    tb_fs_result result = {0};
    TB_ASSERT(directory != NULL && directory->state != NULL);
    TB_ASSERT(required != NULL);
    TB_ASSERT(name != NULL || capacity == 0);
    *required = 0;
    state = directory->state;
    for (;;)
    {
        if (state->ended)
        {
            return TB_FS_END;
        }
        if (!state->pending)
        {
            if (!FindNextFileW(state->handle, &state->entry))
            {
                if (GetLastError() == ERROR_NO_MORE_FILES)
                {
                    state->ended = true;
                    return TB_FS_END;
                }
                return tb_fs_last_error();
            }
            state->pending = true;
        }
        if (state->entry.cFileName[0] == L'.' &&
            (state->entry.cFileName[1] == L'\0' ||
             (state->entry.cFileName[1] == L'.' && state->entry.cFileName[2] == L'\0')))
        {
            state->pending = false;
            continue;
        }
        break;
    }
    result = tb_fs_windows_utf8(state->entry.cFileName, &text);
    if (result == TB_FS_OK)
    {
        result = tb_native_fs_output(text, name, capacity, required);
    }
    free(text);
    if (result == TB_FS_INVALID || (result == TB_FS_OK && name != NULL))
    {
        state->pending = false;
    }
    return result;
}
tb_fs_result tb_directory_close(tb_directory *directory)
{
    tb_native_directory *state = NULL;
    tb_fs_result result = {0};
    TB_ASSERT(directory != NULL);
    state = directory->state;
    directory->state = NULL;
    if (state == NULL)
    {
        return TB_FS_OK;
    }
    if (state->handle != INVALID_HANDLE_VALUE && !FindClose(state->handle))
    {
        result = tb_fs_last_error();
    }
    free(state);
    return result;
}
tb_fs_result tb_native_fs_location(tb_fs_location location, char **path)
{
    WCHAR *wide = NULL;
    PWSTR known = NULL;
    DWORD count = {0};
    tb_fs_result result = {0};
    *path = NULL;
    wide = malloc(32768 * sizeof(WCHAR));
    if (wide == NULL)
    {
        return TB_FS_NO_MEMORY;
    }
    switch (location)
    {
    case TB_FS_EXECUTABLE:
        count = GetModuleFileNameW(NULL, wide, 32768);
        if (count == 0)
        {
            result = tb_fs_last_error();
            goto cleanup;
        }
        if (count >= 32768)
        {
            result = TB_FS_TOO_LARGE;
            goto cleanup;
        }
        while (count > 0 && wide[count - 1] != L'\\')
        {
            count--;
        }
        if (count == 0)
        {
            result = TB_FS_INVALID;
            goto cleanup;
        }
        wide[count] = L'\0';
        break;
    case TB_FS_TEMP:
        count = GetTempPathW(32768, wide);
        if (count == 0)
        {
            result = tb_fs_last_error();
            goto cleanup;
        }
        if (count >= 32768)
        {
            result = TB_FS_TOO_LARGE;
            goto cleanup;
        }
        break;
    case TB_FS_USER_DATA:
    case TB_FS_USER_CONFIG:
    case TB_FS_USER_CACHE: {
        HRESULT answer = {0};
        answer = SHGetKnownFolderPath(location == TB_FS_USER_CACHE ? &FOLDERID_LocalAppData
                                                                   : &FOLDERID_RoamingAppData,
                                      0, NULL, &known);
        if (FAILED(answer))
        {
            result = TB_FS_OS_ERROR;
            goto cleanup;
        }
        result = tb_fs_windows_utf8(known, path);
        goto cleanup;
    }
    default:
        result = TB_FS_INVALID;
        goto cleanup;
    }
    result = tb_fs_windows_utf8(wide, path);
cleanup:
    CoTaskMemFree(known);
    free(wide);
    return result;
}

tb_path_style tb_fs_native_style(void)
{
    return TB_PATH_WINDOWS;
}
bool tb_native_fs_separator(char byte)
{
    return byte == '/' || byte == '\\';
}
