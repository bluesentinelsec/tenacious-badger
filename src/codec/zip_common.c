/* What the ZIP reader and writer share: calendar dates and result names. */
#include "badger/zip.h"

#include "zip_internal.h"

/* Howard Hinnant's algorithms, on eras of 400 years that start in March. */
int64_t tb_zip_days_from_civil(int64_t year, unsigned month, unsigned day)
{
    const int64_t shifted = month <= 2 ? year - 1 : year;
    const int64_t era = (shifted >= 0 ? shifted : shifted - 399) / 400;
    const int64_t year_of_era = shifted - era * 400;
    const int64_t month_index = month > 2 ? (int64_t)month - 3 : (int64_t)month + 9;
    const int64_t day_of_year = (153 * month_index + 2) / 5 + (int64_t)day - 1;
    const int64_t day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;
}

void tb_zip_civil_from_days(int64_t days, int64_t *year, unsigned *month, unsigned *day)
{
    const int64_t shifted = days + 719468;
    const int64_t era = (shifted >= 0 ? shifted : shifted - 146096) / 146097;
    const int64_t day_of_era = shifted - era * 146097;
    const int64_t year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const int64_t day_of_year =
        day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const int64_t month_index = (5 * day_of_year + 2) / 153;
    *day = (unsigned)(day_of_year - (153 * month_index + 2) / 5 + 1);
    *month = (unsigned)(month_index < 10 ? month_index + 3 : month_index - 9);
    *year = year_of_era + era * 400 + (*month <= 2 ? 1 : 0);
}

const char *tb_zip_result_name(tb_zip_result result)
{
    switch (result)
    {
    case TB_ZIP_OK:
        return "ok";
    case TB_ZIP_INVALID:
        return "invalid";
    case TB_ZIP_MALFORMED:
        return "malformed";
    case TB_ZIP_TOO_LARGE:
        return "too large";
    case TB_ZIP_UNSUPPORTED:
        return "unsupported";
    case TB_ZIP_DUPLICATE_NAME:
        return "duplicate name";
    case TB_ZIP_INPUT_FAILED:
        return "input failed";
    case TB_ZIP_OUTPUT_FAILED:
        return "output failed";
    default:
        return "unknown result";
    }
}
