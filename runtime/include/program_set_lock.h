/* program_set_lock.h — one program of a multi-program set at a time.
 *
 * A set whose discs boot different programs ships one executable per program
 * in one folder, with one shared saves folder (Resident Evil 2: a Leon exe and
 * a Claire exe). Each running program keeps the memory cards in memory and
 * writes them back, so two of them running at once can lose a save. The
 * second one to start must refuse.
 *
 * The lock is a file in the shared saves folder, held open for the life of the
 * process. The operating system releases it when the process ends, however it
 * ends, so a crash never leaves the set locked.
 */
#ifndef PSXRECOMP_PROGRAM_SET_LOCK_H
#define PSXRECOMP_PROGRAM_SET_LOCK_H

#ifdef __cplusplus
extern "C" {
#endif

#define PSX_PROGRAM_SET_LOCK_NAME ".program-set.lock"

/* Take the lock in `saves_dir`.
 *   1  this process holds it (now, or from an earlier call);
 *   0  another process holds it: refuse to start;
 *  -1  the lock file could not be opened for another reason (read-only
 *      folder, bad path). The caller should carry on: a lock problem must not
 *      stop a player whose folder is otherwise usable. */
int psx_program_set_lock_acquire(const char *saves_dir);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_PROGRAM_SET_LOCK_H */
