import { useCallback, useEffect, useState } from 'react'
import { useNavigate, useLocation } from 'react-router-dom'
import { motion, AnimatePresence } from 'framer-motion'
import {
  MessageSquare,
  Settings,
  Wrench,
  Bot,
  Zap,
  Monitor,
} from 'lucide-react'
import { cn } from '../../lib/utils'

const NAV_ITEMS = [
  { to: '/chat/default', icon: MessageSquare },
  { to: '/tools', icon: Wrench },
  { to: '/agents', icon: Bot },
  { to: '/skills', icon: Zap },
  { to: '/config', icon: Settings },
  { to: '/system', icon: Monitor },
]

const ARC_RADIUS = 120
// Centered arc: items spread symmetrically above bottom center
const ARC_START = -150
const ARC_END = -30

function isActive(pathname: string, itemPath: string): boolean {
  if (itemPath.startsWith('/chat')) return pathname.startsWith('/chat')
  return pathname === itemPath || pathname.startsWith(itemPath + '/')
}

export function RadialDial() {
  const [open, setOpen] = useState(false)
  const navigate = useNavigate()
  const location = useLocation()

  const handleSelect = useCallback((to: string) => {
    navigate(to)
    setOpen(false)
  }, [navigate])

  // Auto-detect: mouse enters bottom hot zone → open
  // Mouse leaves the arc area → close
  useEffect(() => {
    const onMove = (e: MouseEvent) => {
      const distFromBottom = window.innerHeight - e.clientY

      if (!open && distFromBottom < 24) {
        setOpen(true)
        return
      }

      if (open && distFromBottom > ARC_RADIUS + 80) {
        setOpen(false)
      }
    }

    window.addEventListener('mousemove', onMove)
    return () => window.removeEventListener('mousemove', onMove)
  }, [open])

  const count = NAV_ITEMS.length
  const span = ARC_END - ARC_START
  const step = count > 1 ? span / (count - 1) : 0

  return (
    <AnimatePresence>
      {open && (
        <>
          {/* Dim backdrop */}
          <motion.div
            className="radial-backdrop"
            initial={{ opacity: 0 }}
            animate={{ opacity: 1 }}
            exit={{ opacity: 0 }}
            transition={{ duration: 0.15 }}
            onClick={() => setOpen(false)}
          />

          {/* Arc items — anchored at bottom center */}
          <div
            className="fixed z-50"
            style={{ bottom: 20, left: '50%', transform: 'translateX(-50%)' }}
          >
            {NAV_ITEMS.map((item, i) => {
              const angle = ARC_START + step * i
              const rad = (angle * Math.PI) / 180
              const x = Math.cos(rad) * ARC_RADIUS
              const y = Math.sin(rad) * ARC_RADIUS
              const Icon = item.icon
              const active = isActive(location.pathname, item.to)

              return (
                <motion.div
                  key={item.to}
                  className={cn('radial-item', active && 'active')}
                  initial={{ opacity: 0, scale: 0, x: 0, y: 0 }}
                  animate={{ opacity: 1, scale: 1, x, y }}
                  exit={{ opacity: 0, scale: 0, x: 0, y: 0 }}
                  transition={{
                    type: 'spring',
                    stiffness: 500,
                    damping: 25,
                    delay: i * 0.03,
                  }}
                  onClick={() => handleSelect(item.to)}
                >
                  <div className="radial-item-icon">
                    <Icon size={20} />
                  </div>
                </motion.div>
              )
            })}
          </div>
        </>
      )}
    </AnimatePresence>
  )
}
