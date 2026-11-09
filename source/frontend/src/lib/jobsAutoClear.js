// How long a finished file job stays on the Files page before it clears itself, in seconds. 0 keeps it.
export const JOBS_AUTOCLEAR_DEFAULT = 5
export const JOBS_AUTOCLEAR_KEY = 'pmgr-jobs-autoclear'
export const JOBS_AUTOCLEAR_OPTIONS = [0, 1, 5, 10, 30, 60, 300]

export function loadJobsAutoClear() {
  try {
    const saved = window.localStorage.getItem(JOBS_AUTOCLEAR_KEY)
    // No saved choice yet means the default; "Never" is a choice and is saved as 0.
    if (saved === null) return JOBS_AUTOCLEAR_DEFAULT
    const seconds = Number(saved)
    return JOBS_AUTOCLEAR_OPTIONS.includes(seconds) ? seconds : JOBS_AUTOCLEAR_DEFAULT
  } catch {
    return JOBS_AUTOCLEAR_DEFAULT
  }
}
