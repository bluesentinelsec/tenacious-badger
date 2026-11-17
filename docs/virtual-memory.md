# Virtual memory

`badger/virtual_memory.h` owns address-space reservations independently of the
core heap. Initialize a region to zero, reserve bytes, commit page-aligned
subranges before accessing them, and release the whole region when done.
Reservations round up to regular pages and do not count as heap allocations.
The base obeys the native allocation granularity. Sizes above PTRDIFF_MAX and
unaligned/out-of-bounds subranges are rejected without modifying the mapping.

Reserve grants no access. Commit grants read/write access and preserves pages
already committed. Decommit discards contents and revokes access; recommit
supplies zeros. All accesses and mapping changes to one region must be externally
synchronized. Independent regions can be manipulated from different threads.
Regions are move-only owners; copying a value does not duplicate a reservation.
After a failed commit/decommit, do not access the affected pages until a commit
succeeds. Release zeros the owner only on success.

Windows uses `VirtualAlloc` and `VirtualFree`. Linux and macOS reserve anonymous
`PROT_NONE` mappings, commit with `mprotect`, and replace an owned subrange with
a fresh inaccessible anonymous mapping to decommit. The replacement is bounded
by the reservation before `MAP_FIXED` is used. This guarantees fresh zeros on
both hosts; Darwin's advisory discard alone does not. POSIX demand paging and
system overcommit mean a successful commit cannot guarantee physical memory
will remain available when every page is subsequently touched.

Page geometry reports an optional explicit large-page size. A nonzero size says
the platform exposes the operation, not that this process has sufficient
privilege or that large pages are free. `tb_vm_allocate_large` reserves and
commits in one step; an unavailable capability returns `TB_VM_UNAVAILABLE` so
the caller can choose regular pages. It never changes privileges or system
configuration. Windows uses `MEM_LARGE_PAGES`; Linux uses the kernel's default
HugeTLB size from `/proc/meminfo`; Intel macOS requests 2 MiB superpages. Apple
Silicon exposes regular pages and OS-managed promotion, not this explicit
superpage operation. Large regions support whole-region release, not partial
commit/decommit.

Tests reserve up to 8 GiB while touching only two pages, check zeroing and
recommit preservation, reject invalid ranges, and exercise optional-large-page
fallback. Child-process probes require an actual memory-access fault after
successful setup when touching reserved or decommitted pages. The default
adapter reports every function as not implemented on unsupported platforms.

Native references: [Windows page management](https://learn.microsoft.com/en-us/windows/win32/memory/working-with-pages),
[Linux mmap](https://man7.org/linux/man-pages/man2/mmap.2.html), and Apple's
[XNU superpage definitions](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/mach/vm_statistics.h).
