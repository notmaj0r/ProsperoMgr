import { useEffect, useRef, useState } from 'react'
import Panel from '../components/Panel.jsx'
import Pill from '../components/Pill.jsx'
import Button from '../components/Button.jsx'
import DropZone from '../components/DropZone.jsx'
import { UploadIcon, SaveIcon, CloseIcon, EditIcon, CheckCircleIcon, UserIcon } from '../components/icons.jsx'
import { api } from '../lib/api.js'
import { useToast } from '../lib/ToastContext.jsx'
import { useI18n } from '../lib/i18n.jsx'

const VIEWPORT = 260 // square crop viewport, px — matches the 260 avatar size

// Drag-to-pan, slider-to-zoom crop, matching the backend's own server-side center-crop —
// but lets the user pick which part of the image that square actually is first.
function Cropper({ src, onCancel, onConfirm, t }) {
  const canvasRef = useRef(null)
  const imgRef = useRef(null)
  const [ready, setReady] = useState(false)
  const [zoom, setZoom] = useState(1)
  const [pan, setPan] = useState({ x: 0, y: 0 })
  const dragRef = useRef(null)

  useEffect(() => {
    const img = new Image()
    img.onload = () => {
      imgRef.current = img
      setZoom(1)
      setPan({ x: 0, y: 0 })
      setReady(true)
    }
    img.src = src
  }, [src])

  const geometry = () => {
    const img = imgRef.current
    const baseScale = VIEWPORT / Math.min(img.naturalWidth, img.naturalHeight)
    const scale = baseScale * zoom
    const w = img.naturalWidth * scale
    const h = img.naturalHeight * scale
    return { scale, w, h }
  }

  const clamp = (p, w, h) => ({
    x: Math.min(0, Math.max(VIEWPORT - w, p.x)),
    y: Math.min(0, Math.max(VIEWPORT - h, p.y)),
  })

  useEffect(() => {
    if (!ready) return
    const { w, h } = geometry()
    setPan((p) => clamp(p, w, h))
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [ready, zoom])

  useEffect(() => {
    if (!ready) return
    const canvas = canvasRef.current
    const ctx = canvas.getContext('2d')
    const { w, h } = geometry()
    ctx.clearRect(0, 0, VIEWPORT, VIEWPORT)
    ctx.drawImage(imgRef.current, pan.x, pan.y, w, h)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [ready, zoom, pan])

  const onPointerDown = (e) => {
    e.currentTarget.setPointerCapture(e.pointerId)
    dragRef.current = { startX: e.clientX, startY: e.clientY, panX: pan.x, panY: pan.y }
  }
  const onPointerMove = (e) => {
    if (!dragRef.current) return
    const { w, h } = geometry()
    const dx = e.clientX - dragRef.current.startX
    const dy = e.clientY - dragRef.current.startY
    setPan(clamp({ x: dragRef.current.panX + dx, y: dragRef.current.panY + dy }, w, h))
  }
  const onPointerUp = () => { dragRef.current = null }

  const confirm = () => {
    const out = document.createElement('canvas')
    out.width = VIEWPORT
    out.height = VIEWPORT
    out.getContext('2d').drawImage(canvasRef.current, 0, 0)
    out.toBlob((blob) => blob && onConfirm(blob), 'image/png')
  }

  return (
    <div className="pmgr-row-in flex flex-col items-center gap-3">
      <div
        className="touch-none overflow-hidden rounded-full border-2 border-accent/50"
        style={{ width: VIEWPORT, height: VIEWPORT, cursor: ready ? 'grab' : 'default' }}
        onPointerDown={onPointerDown}
        onPointerMove={onPointerMove}
        onPointerUp={onPointerUp}
        onPointerLeave={onPointerUp}
      >
        <canvas ref={canvasRef} width={VIEWPORT} height={VIEWPORT} />
      </div>

      <div className="flex w-full max-w-xs items-center gap-2">
        <span className="text-xs text-text-dim">{t('profile.cropZoom')}</span>
        <input
          type="range"
          min="1"
          max="3"
          step="0.01"
          value={zoom}
          onChange={(e) => setZoom(parseFloat(e.target.value))}
          className="flex-1"
        />
      </div>
      <p className="text-xs text-text-dim">{t('profile.cropHint')}</p>

      <div className="flex gap-2">
        <Button tone="neutral" icon={<CloseIcon size={15} />} onClick={onCancel}>{t('common.cancel')}</Button>
        <Button tone="primary" icon={<SaveIcon size={15} />} onClick={confirm}>{t('profile.useThisCrop')}</Button>
      </div>
    </div>
  )
}

export default function AvatarView() {
  const { t } = useI18n()
  const [who, setWho] = useState(null)
  const [previewKey, setPreviewKey] = useState(0)
  const [hasPreview, setHasPreview] = useState(false)
  const [uploading, setUploading] = useState(false)
  const [applying, setApplying] = useState(false)
  const [applied, setApplied] = useState(false)
  const [cropSrc, setCropSrc] = useState(null)
  const [renaming, setRenaming] = useState(false)
  const [nameInput, setNameInput] = useState('')
  const [savingName, setSavingName] = useState(false)
  const fileInputRef = useRef(null)
  const showToast = useToast()

  const loadWho = () => {
    api.avatar.whoami().then(setWho).catch(() => setWho({ ok: false, error: t('profile.failedToReachBackend') }))
  }

  useEffect(loadWho, [])

  const startRename = () => {
    setNameInput(who?.user_name || '')
    setRenaming(true)
  }

  const saveRename = () => {
    const name = nameInput.trim()
    if (!name) return
    setSavingName(true)
    api.avatar
      .rename(name)
      .then(() => {
        showToast(t('profile.renamedToast', { name }))
        setRenaming(false)
        loadWho()
      })
      .catch((e) => showToast(e.message, 'err'))
      .finally(() => setSavingName(false))
  }

  const doUpload = (blob) => {
    setUploading(true)
    api.avatar
      .upload(blob)
      .then(() => {
        setHasPreview(true)
        setApplied(false)
        setPreviewKey((k) => k + 1)
        showToast(t('profile.previewReadyToast'))
      })
      .catch((e) => showToast(e.message, 'err'))
      .finally(() => setUploading(false))
  }

  const pickFile = (file) => {
    if (!file) return
    setCropSrc(URL.createObjectURL(file))
  }

  const onPick = (e) => {
    const file = e.target.files?.[0]
    e.target.value = ''
    pickFile(file)
  }

  const confirmCrop = (blob) => {
    setCropSrc(null)
    doUpload(blob)
  }

  const apply = () => {
    setApplying(true)
    api.avatar
      .apply()
      .then(() => {
        setApplied(true)
        showToast(t('profile.appliedToast'))
      })
      .catch((e) => showToast(e.message, 'err'))
      .finally(() => setApplying(false))
  }

  return (
    <div className="space-y-4">
      {/* Hero: who this whole page acts on. */}
      <Panel title={t('profile.title')}>
        {who ? (
          who.ok ? (
            <div className="flex flex-wrap items-center gap-4">
              <img
                key={applied ? `applied-${previewKey}` : 'current'}
                src={`${api.avatar.userIconUrl(who.user_id_hex)}${applied ? `&applied=${previewKey}` : ''}`}
                alt=""
                onError={(e) => { e.target.style.display = 'none' }}
                className="h-16 w-16 shrink-0 rounded-full border border-accent/30 object-cover"
              />
              <div className="min-w-0 flex-1">
                {renaming ? (
                  <div className="flex items-center gap-2">
                    <input
                      autoFocus
                      value={nameInput}
                      onChange={(e) => setNameInput(e.target.value)}
                      onKeyDown={(e) => e.key === 'Enter' && saveRename()}
                      maxLength={16}
                      className="min-w-0 max-w-48 rounded-md border border-panel-border bg-bg px-2 py-1.5 text-sm font-semibold text-text outline-none focus:border-accent"
                    />
                    <Button size="sm" tone="primary" icon={<SaveIcon size={13} />} onClick={saveRename} disabled={savingName || !nameInput.trim()}>
                      {savingName ? t('profile.saving') : t('profile.save')}
                    </Button>
                    <Button size="sm" tone="neutral" icon={<CloseIcon size={13} />} onClick={() => setRenaming(false)} />
                  </div>
                ) : (
                  <div className="flex items-center gap-2">
                    <span className="truncate text-lg font-bold text-text">{who.user_name}</span>
                    <Button size="icon" tone="ghost" title={t('profile.rename')} onClick={startRename} icon={<EditIcon size={14} />} />
                  </div>
                )}
                <div className="mt-0.5 flex items-center gap-2">
                  <Pill tone="ok" live>{t('profile.signedIn')}</Pill>
                  <span className="truncate text-xs text-text-dim" title={who.user_id_hex} dir="ltr">{who.user_id_hex}</span>
                </div>
              </div>
            </div>
          ) : (
            <div className="text-sm text-text-dim">{who.error}</div>
          )
        ) : (
          <div className="pmgr-skeleton h-4 w-40 rounded" />
        )}
        <p className="mt-3 text-xs text-text-dim">{t('profile.idCap')}</p>
      </Panel>

      {/* One coordinated flow: choose -> crop -> preview -> apply. Only one
          step is ever visible at a time. */}
      <Panel
        title={t('profile.changeAvatarTitle')}
        actions={
          !cropSrc && (
            <>
              <Button tone="primary" icon={<UploadIcon size={15} />} onClick={() => fileInputRef.current?.click()} disabled={uploading}>
                {uploading ? t('profile.processing') : hasPreview ? t('profile.chooseDifferent') : t('profile.chooseImage')}
              </Button>
              <input ref={fileInputRef} type="file" accept="image/*" hidden onChange={onPick} />
            </>
          )
        }
      >
        {applied && (
          <div className="mb-4 flex items-start gap-3 rounded-xl border border-ok/30 bg-ok/10 p-3.5">
            <span className="mt-0.5 grid h-8 w-8 shrink-0 place-items-center rounded-lg bg-ok/15 text-ok"><CheckCircleIcon size={16} /></span>
            <div className="min-w-0">
              <div className="text-sm font-bold text-text">{t('profile.appliedTitle')}</div>
              <div className="text-xs text-text-dim">{t('profile.appliedDesc')}</div>
            </div>
          </div>
        )}

        {cropSrc ? (
          <Cropper src={cropSrc} onCancel={() => setCropSrc(null)} onConfirm={confirmCrop} t={t} />
        ) : hasPreview ? (
          <div className="pmgr-row-in flex flex-wrap items-end gap-4">
            <img
              key={previewKey}
              src={api.avatar.previewUrl(440)}
              alt={t('profile.avatarPreviewAlt')}
              className="h-32 w-32 rounded-full border border-panel-border object-cover"
            />
            <div className="flex flex-col gap-2">
              <p className="text-sm text-text-dim">{t('profile.previewOnly')}</p>
              <Button
                tone="primary"
                sheen
                className="w-fit"
                onClick={apply}
                disabled={applying || !who?.ok}
                icon={<SaveIcon size={15} />}
              >
                {applying ? t('profile.applying') : t('profile.apply')}
              </Button>
              {!who?.ok && <span className="text-xs text-err">{who?.error}</span>}
            </div>
          </div>
        ) : (
          <DropZone onFiles={(files) => pickFile(files[0])}>
            <div className="flex flex-col items-center gap-2 rounded-xl border border-dashed border-panel-border bg-bg/40 px-4 py-8 text-center">
              <span className="grid h-11 w-11 place-items-center rounded-xl bg-accent/10 text-accent"><UserIcon size={20} /></span>
              <p className="max-w-sm text-sm text-text-dim">{t('profile.dropHint')}</p>
            </div>
          </DropZone>
        )}
      </Panel>
    </div>
  )
}
