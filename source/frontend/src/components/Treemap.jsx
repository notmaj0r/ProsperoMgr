import { useEffect, useMemo, useRef, useState } from 'react'

// WinDirStat-style squarified treemap on a <canvas> — a scan can put thousands of blocks
// on screen, which is far cheaper to paint than the same number of DOM nodes.
// Two levels: the current folder's children, and their children nested inside.

const HUES = [212, 28, 140, 268, 350, 186, 48, 100, 320, 240]
const PAD = 2 // gap between a folder's outline and its nested children
const MIN_NEST = 14 // a block smaller than this (px) is drawn solid instead of nested

function worst(row, rowSum, side) {
  let max = 0
  let min = Infinity
  for (const r of row) {
    if (r.area > max) max = r.area
    if (r.area < min) min = r.area
  }
  const s2 = side * side
  const sum2 = rowSum * rowSum
  return Math.max((s2 * max) / sum2, sum2 / (s2 * min))
}

// Bruls/Huizing/van Wijk squarify. `items` need a positive `value`; output keeps their order.
export function squarify(items, x, y, w, h) {
  const total = items.reduce((sum, item) => sum + item.value, 0)
  if (total <= 0 || w <= 0 || h <= 0) return []
  const scale = (w * h) / total
  const queue = items.map((item) => ({ item, area: item.value * scale }))
  const out = []
  let row = []
  let rowSum = 0

  const layoutRow = () => {
    if (!row.length) return
    if (w >= h) {
      // Column along the left edge.
      const colW = rowSum / h
      let cy = y
      for (const r of row) {
        const rh = r.area / colW
        out.push({ item: r.item, x, y: cy, w: colW, h: rh })
        cy += rh
      }
      x += colW
      w -= colW
    } else {
      const rowH = rowSum / w
      let cx = x
      for (const r of row) {
        const rw = r.area / rowH
        out.push({ item: r.item, x: cx, y, w: rw, h: rowH })
        cx += rw
      }
      y += rowH
      h -= rowH
    }
    row = []
    rowSum = 0
  }

  for (const entry of queue) {
    const side = Math.min(w, h)
    if (!row.length || worst([...row, entry], rowSum + entry.area, side) <= worst(row, rowSum, side)) {
      row.push(entry)
      rowSum += entry.area
    } else {
      layoutRow()
      row.push(entry)
      rowSum += entry.area
    }
  }
  layoutRow()
  return out
}

function colorFor(hue, depth, index, kind) {
  if (kind === 'rest' || kind === 'mount') return { h: 215, s: 8, l: 46 }
  if (kind === 'files') return { h: hue, s: 22, l: 50 }
  const l = depth === 0 ? 50 : 44 + ((index * 7) % 18)
  return { h: hue, s: kind === 'file' ? 48 : 62, l }
}

function hsl({ h, s, l }, dl = 0) {
  return `hsl(${h}, ${s}%, ${Math.max(0, Math.min(100, l + dl))}%)`
}

// Light from the top-left, like WinDirStat's cushions — gives nested blocks a visible edge
// without needing a border on every one of them.
function paintCushion(ctx, r, color) {
  if (r.w < 1 || r.h < 1) return
  const g = ctx.createRadialGradient(
    r.x + r.w * 0.3, r.y + r.h * 0.3, 0,
    r.x + r.w * 0.5, r.y + r.h * 0.5, Math.max(r.w, r.h) * 0.75,
  )
  g.addColorStop(0, hsl(color, 18))
  g.addColorStop(0.6, hsl(color, 0))
  g.addColorStop(1, hsl(color, -16))
  ctx.fillStyle = g
  ctx.fillRect(r.x, r.y, r.w, r.h)
}

function layout(node, metric, width, height) {
  const blocks = []
  const value = (n) => metric(n)
  // Squarify only gives good aspect ratios largest-first, under whichever measure is shown.
  const sorted = (list) => (list || []).filter((c) => value(c) > 0).sort((a, b) => value(b) - value(a))
  const children = sorted(node.children)
  const top = squarify(children.map((c) => ({ value: value(c), node: c })), 0, 0, width, height)
  top.forEach((r, i) => {
    const hue = HUES[i % HUES.length]
    const child = r.item.node
    const block = { x: r.x, y: r.y, w: r.w, h: r.h, node: child, depth: 0, hue, color: colorFor(hue, 0, i, child.type), parentNode: null }
    blocks.push(block)
    const grand = sorted(child.children)
    if (child.type === 'dir' && grand.length && r.w > MIN_NEST && r.h > MIN_NEST) {
      block.nested = true
      const inner = squarify(
        grand.map((c) => ({ value: value(c), node: c })),
        r.x + PAD, r.y + PAD, r.w - PAD * 2, r.h - PAD * 2,
      )
      inner.forEach((ir, j) => {
        blocks.push({
          x: ir.x, y: ir.y, w: ir.w, h: ir.h, node: ir.item.node, depth: 1, hue,
          color: colorFor(hue, 1, j, ir.item.node.type), parentNode: child,
        })
      })
    }
  })
  return blocks
}

export default function Treemap({ node, metric, formatSize, describe, onOpen, className = '' }) {
  const wrapRef = useRef(null)
  const canvasRef = useRef(null)
  const [size, setSize] = useState({ w: 0, h: 0 })
  const [hover, setHover] = useState(null) // { block, x, y }

  useEffect(() => {
    const wrap = wrapRef.current
    if (!wrap) return undefined
    const measure = () => setSize({ w: Math.floor(wrap.clientWidth), h: Math.floor(wrap.clientHeight) })
    measure()
    // ResizeObserver arrived in Safari 13.1 — older PS5 browsers fall back to window resizes.
    if (typeof window.ResizeObserver === 'function') {
      const observer = new window.ResizeObserver(measure)
      observer.observe(wrap)
      return () => observer.disconnect()
    }
    window.addEventListener('resize', measure)
    return () => window.removeEventListener('resize', measure)
  }, [])

  const blocks = useMemo(
    () => (node && size.w > 0 && size.h > 0 ? layout(node, metric, size.w, size.h) : []),
    [node, metric, size.w, size.h],
  )

  // A hovered block from the previous layout would outline the wrong place.
  useEffect(() => setHover(null), [blocks])

  useEffect(() => {
    const canvas = canvasRef.current
    if (!canvas || !size.w || !size.h) return
    const dpr = window.devicePixelRatio || 1
    canvas.width = size.w * dpr
    canvas.height = size.h * dpr
    const ctx = canvas.getContext('2d')
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0)
    ctx.clearRect(0, 0, size.w, size.h)
    const styles = window.getComputedStyle(document.documentElement)
    const gap = styles.getPropertyValue('--color-panel').trim() || '#10161f'

    for (const b of blocks) {
      if (b.depth === 0 && b.nested) {
        ctx.fillStyle = hsl(b.color, -22)
        ctx.fillRect(b.x, b.y, b.w, b.h)
      } else {
        paintCushion(ctx, b, b.color)
        if (b.node.type === 'rest' || b.node.type === 'files' || b.node.type === 'mount') {
          // Hatched: an aggregate, not a single thing you can open.
          ctx.save()
          ctx.beginPath()
          ctx.rect(b.x, b.y, b.w, b.h)
          ctx.clip()
          ctx.strokeStyle = 'rgba(255,255,255,0.12)'
          ctx.lineWidth = 1
          for (let d = -b.h; d < b.w; d += 6) {
            ctx.beginPath()
            ctx.moveTo(b.x + d, b.y + b.h)
            ctx.lineTo(b.x + d + b.h, b.y)
            ctx.stroke()
          }
          ctx.restore()
        }
      }
      ctx.strokeStyle = gap
      ctx.lineWidth = b.depth === 0 ? 1.5 : 0.75
      ctx.strokeRect(b.x, b.y, b.w, b.h)
    }

    // Labels last so nested blocks never paint over them.
    ctx.textBaseline = 'top'
    ctx.font = '600 12px system-ui, -apple-system, "Segoe UI", sans-serif'
    for (const b of blocks) {
      if (b.depth !== 0 || b.w < 56 || b.h < 22) continue
      const label = describe(b.node).name
      const sizeText = formatSize(metric(b.node))
      const maxW = b.w - 10
      let text = label
      while (text.length > 1 && ctx.measureText(text).width > maxW) text = text.slice(0, -2)
      if (text !== label) text = `${text.slice(0, -1)}…`
      ctx.fillStyle = 'rgba(0,0,0,0.55)'
      const tw = Math.min(maxW, ctx.measureText(text).width) + 8
      const showSize = b.h >= 40
      ctx.fillRect(b.x + 3, b.y + 3, Math.max(tw, showSize ? ctx.measureText(sizeText).width + 8 : 0), showSize ? 32 : 18)
      ctx.fillStyle = '#fff'
      ctx.fillText(text, b.x + 7, b.y + 6)
      if (showSize) {
        ctx.fillStyle = 'rgba(255,255,255,0.75)'
        ctx.fillText(sizeText, b.x + 7, b.y + 20)
      }
    }
  }, [blocks, size.w, size.h, describe, formatSize, metric])

  const blockAt = (event) => {
    const rect = canvasRef.current.getBoundingClientRect()
    const x = event.clientX - rect.left
    const y = event.clientY - rect.top
    let found = null
    for (const b of blocks) {
      if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h && (!found || b.depth >= found.depth)) found = b
    }
    return found ? { block: found, x, y } : null
  }

  const onMove = (event) => {
    const hit = blockAt(event)
    setHover((prev) => (prev?.block === hit?.block ? (hit ? { ...prev, x: hit.x, y: hit.y } : prev) : hit))
  }

  const onClick = (event) => {
    const hit = blockAt(event)
    if (!hit) return
    const { block } = hit
    const target = block.node.type === 'dir' ? block : null
    if (target && block.parentNode) onOpen([block.parentNode, block.node])
    else if (target) onOpen([block.node])
    else if (block.parentNode) onOpen([block.parentNode])
  }

  const total = node ? metric(node) : 0
  const tip = hover && describe(hover.block.node)
  const tipLeft = hover ? Math.min(hover.x + 14, Math.max(0, size.w - 260)) : 0
  const tipTop = hover ? (hover.y + 90 > size.h ? hover.y - 80 : hover.y + 14) : 0

  return (
    <div ref={wrapRef} className={`relative overflow-hidden rounded-lg border border-panel-border bg-bg/40 ${className}`}>
      <canvas
        ref={canvasRef}
        style={{ width: size.w, height: size.h, display: 'block', cursor: hover && hover.block.node.type !== 'rest' ? 'pointer' : 'default' }}
        onMouseMove={onMove}
        onMouseLeave={() => setHover(null)}
        onClick={onClick}
      />
      {hover && (
        // Outline as an overlay so hovering never repaints thousands of canvas blocks.
        <div
          className="pointer-events-none absolute border-2 border-white"
          style={{ left: hover.block.x, top: hover.block.y, width: hover.block.w, height: hover.block.h }}
        />
      )}
      {tip && (
        <div
          className="pointer-events-none absolute z-10 w-64 rounded-lg border border-panel-border bg-panel/95 px-3 py-2 text-xs shadow-xl"
          style={{ left: tipLeft, top: tipTop }}
        >
          <div className="truncate font-semibold text-text">{tip.name}</div>
          {hover.block.parentNode && <div className="truncate text-text-dim">{describe(hover.block.parentNode).name} /</div>}
          <div className="mt-1 flex justify-between gap-2 text-text-dim">
            <span className="pmgr-mono">{formatSize(metric(hover.block.node))}</span>
            <span className="pmgr-mono">{total > 0 ? `${((metric(hover.block.node) / total) * 100).toFixed(1)}%` : ''}</span>
          </div>
          {tip.detail && <div className="mt-0.5 text-text-dim">{tip.detail}</div>}
        </div>
      )}
    </div>
  )
}
