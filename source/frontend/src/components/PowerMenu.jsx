import { useCallback, useState } from 'react'
import ActionMenu from './ActionMenu.jsx'
import { ConfirmDialog } from './Dialog.jsx'
import SegmentedControl from './SegmentedControl.jsx'
import { ClockIcon, MoonIcon, PowerIcon, RefreshIcon } from './icons.jsx'
import { api } from '../lib/api.js'
import { useToast } from '../lib/ToastContext.jsx'
import { useI18n } from '../lib/i18n.jsx'
import { usePolling } from '../lib/usePolling.js'

const ACTIONS = ['standby', 'reboot', 'shutdown']
const ACTION_ICONS = { standby: MoonIcon, reboot: RefreshIcon, shutdown: PowerIcon }
const TIMER_MINUTES = [15, 30, 60, 120, 240]
const MAX_MINUTES = 1440

function ScheduleDialog({ onClose, onSaved }) {
  const { t } = useI18n()
  const showToast = useToast()
  const [action, setAction] = useState('standby')
  const [mode, setMode] = useState('timer')
  const [minutes, setMinutes] = useState(30)

  const save = () => {
    if (mode === 'timer' && !(minutes >= 1 && minutes <= MAX_MINUTES)) throw new Error(t('power.invalidMinutes'))
    return api.power.setSchedule({ action, mode, minutes }).then(() => {
      showToast(t('power.scheduledToast'))
      onSaved()
    })
  }

  return (
    <ConfirmDialog
      title={t('power.scheduleTitle')}
      description={t('power.scheduleDesc')}
      confirmLabel={t('power.scheduleConfirm')}
      cancelLabel={t('common.cancel')}
      onClose={onClose}
      onConfirm={save}
    >
      <div className="flex flex-col gap-3">
        <SegmentedControl
          label={t('power.scheduleAction')}
          value={action}
          onChange={setAction}
          options={ACTIONS.map((id) => ({ id, label: t(`power.${id}`) }))}
        />
        <SegmentedControl
          label={t('power.scheduleWhen')}
          value={mode}
          onChange={setMode}
          options={[{ id: 'timer', label: t('power.afterTime') }, { id: 'idle', label: t('power.whenJobsFinish') }]}
        />
        {mode === 'timer' && (
          <>
            <SegmentedControl
              label={t('power.afterTime')}
              value={minutes}
              onChange={setMinutes}
              options={TIMER_MINUTES.map((m) => ({ id: m, label: t('power.minutes', { minutes: m }) }))}
            />
            <label className="flex items-center gap-2 text-sm text-text-dim">
              {t('power.customMinutes')}
              <input
                type="number"
                min="1"
                max={MAX_MINUTES}
                value={minutes}
                onChange={(e) => setMinutes(e.target.value === '' ? '' : Number(e.target.value))}
                className="w-24 rounded-md border border-panel-border bg-bg px-2 py-1.5 text-sm text-text outline-none focus:border-accent"
              />
            </label>
          </>
        )}
      </div>
    </ConfirmDialog>
  )
}

// Same three choices as the Windows power button. Each one interrupts running games and
// transfers, so it always goes through a confirmation that says what will happen.
export default function PowerMenu() {
  const { t } = useI18n()
  const showToast = useToast()
  const [pending, setPending] = useState(null)
  const [scheduling, setScheduling] = useState(false)
  const [schedule, setSchedule] = useState(null)

  const refreshSchedule = useCallback(() => {
    api.power.schedule().then(setSchedule).catch(() => {})
  }, [])

  usePolling(refreshSchedule, 15000)

  const items = ACTIONS.map((action) => {
    const Icon = ACTION_ICONS[action]
    return {
      label: t(`power.${action}`),
      icon: <Icon size={15} />,
      tone: action === 'shutdown' ? 'danger' : undefined,
      onClick: () => setPending(action),
    }
  })

  items.push('-')
  if (schedule?.active) {
    const when = schedule.mode === 'idle'
      ? t('power.whenJobsFinish')
      : t('power.inMinutes', { minutes: Math.max(1, Math.ceil(schedule.seconds_left / 60)) })
    items.push({
      label: t('power.cancelScheduled', { action: t(`power.${schedule.action}`), when }),
      icon: <ClockIcon size={15} />,
      onClick: () => api.power.cancelSchedule().then(() => {
        showToast(t('power.scheduleCanceled'))
        refreshSchedule()
      }),
    })
  } else {
    items.push({ label: t('power.schedule'), icon: <ClockIcon size={15} />, onClick: () => setScheduling(true) })
  }

  const confirm = () =>
    api.power.run(pending).then(() => showToast(t(`power.${pending}Sent`)))

  return (
    <>
      <ActionMenu
        label={t('power.title')}
        items={items}
        icon={
          <>
            <PowerIcon size={15} />
            {schedule?.active && <span className="pmgr-live-dot absolute -right-0.5 -top-0.5 text-warn" />}
          </>
        }
        buttonClassName="pmgr-icon-btn pmgr-power-btn relative grid h-8 w-8 shrink-0 place-items-center rounded-full border border-panel-border text-text-dim hover:border-err/50 hover:text-err"
      />
      {pending && (
        <ConfirmDialog
          title={t(`power.${pending}Title`)}
          description={t(`power.${pending}Desc`)}
          tone="warn"
          confirmLabel={t(`power.${pending}`)}
          cancelLabel={t('common.cancel')}
          onClose={() => setPending(null)}
          onConfirm={confirm}
        />
      )}
      {scheduling && (
        <ScheduleDialog
          onClose={() => setScheduling(false)}
          onSaved={refreshSchedule}
        />
      )}
    </>
  )
}
