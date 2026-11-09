// Lets the browser's Back (the controller's O button on the console) close the top modal or menu
// instead of leaving the page. Each open layer owns one history entry.
import { useEffect, useRef } from 'react'

const layers = []
let layerCount = 0
let skipPops = 0

window.addEventListener('popstate', () => {
  if (skipPops > 0) {
    skipPops--
    return
  }
  // Forward onto the entry of a layer that already closed must not close another one.
  const landed = window.history.state?.pmgrLayer
  if (landed !== undefined && !layers.some((layer) => layer.key === landed)) return
  layers.pop()?.close()
})

export function pushBackLayer(close) {
  const layer = { key: ++layerCount, close }
  layers.push(layer)
  window.history.pushState({ pmgrLayer: layer.key }, '')
  return () => {
    const at = layers.indexOf(layer)
    if (at < 0) return
    layers.splice(at, 1)
    // Only undo our own entry; if the page moved on meanwhile, the history is left alone.
    if (window.history.state?.pmgrLayer === layer.key) {
      skipPops++
      window.history.back()
    }
  }
}

// Keeps a layer open while `active` is true, so Back (O on the console) undoes it instead of leaving
// the page. With `repeat`, the layer comes back after each Back, for steps that can be taken many times.
export function useBackLayer(active, onBack, repeat = false) {
  const ref = useRef(onBack)
  ref.current = onBack
  useEffect(() => {
    if (!active) return undefined
    let release
    const arm = () => {
      release = pushBackLayer(() => {
        ref.current()
        if (repeat) arm()
      })
    }
    arm()
    return () => release()
  }, [active]) // eslint-disable-line react-hooks/exhaustive-deps
}
