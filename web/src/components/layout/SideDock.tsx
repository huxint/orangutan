import { useCallback, useEffect, useRef, useState } from 'react'
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
  { to: '/chat/default', icon: MessageSquare, label: 'Chat' },
  { to: '/tools', icon: Wrench, label: 'Tools' },
  { to: '/agents', icon: Bot, label: 'Agents' },
  { to: '/skills', icon: Zap, label: 'Skills' },
  { to: '/config', icon: Settings, label: 'Config' },
  { to: '/system', icon: Monitor, label: 'System' },
]

function isActive(pathname: string, itemPath: string): boolean {
  if (itemPath.startsWith('/chat')) return pathname.startsWith('/chat')
  return pathname === itemPath || pathname.startsWith(itemPath + '/')
}

export function SideDock() {
  const [visible, setVisible] = useState(false)
  const [hoveredItem, setHoveredItem] = useState<string | null>(null)
  const navigate = useNavigate()
  const location = useLocation()
  const dockRef = useRef<HTMLDivElement>(null)
  const hideTimerRef = useRef<ReturnType<typeof setTimeout>>(undefined)

  // Preview navigation on hover — navigate immediately for real-time preview
  const handleItemHover = useCallback((to: string) => {
    setHoveredItem(to)
    navigate(to, { replace: true })
  }, [navigate])

  const handleItemClick = useCallback((to: string) => {
    navigate(to)
    setVisible(false)
    setHoveredItem(null)
  }, [navigate])

  // Show dock when mouse approaches left edge
  useEffect(() => {
    const onMove = (e: MouseEvent) => {
      if (e.clientX < 8 && !visible) {
        clearTimeout(hideTimerRef.current)
        setVisible(true)
      }
    }
    window.addEventListener('mousemove', onMove)
    return () => window.removeEventListener('mousemove', onMove)
  }, [visible])

  // Hide dock when mouse leaves
  const handleMouseLeave = useCallback(() => {
    hideTimerRef.current = setTimeout(() => {
      setVisible(false)
      setHoveredItem(null)
    }, 300)
  }, [])

  const handleMouseEnter = useCallback(() => {
    clearTimeout(hideTimerRef.current)
  }, [])

  return (
    <>
      {/* Subtle edge indicator — always visible */}
      <div className="fixed left-0 top-1/2 -translate-y-1/2 z-40 w-[3px] h-16 rounded-r-full bg-accent/20 transition-opacity duration-300"
        style={{ opacity: visible ? 0 : 0.6 }}
      />

      <AnimatePresence>
        {visible && (
          <motion.div
            ref={dockRef}
            className="fixed left-0 top-0 bottom-0 z-50 flex items-center"
            initial={{ x: -60 }}
            animate={{ x: 0 }}
            exit={{ x: -60 }}
            transition={{ type: 'spring', stiffness: 400, damping: 30 }}
            onMouseEnter={handleMouseEnter}
            onMouseLeave={handleMouseLeave}
          >
            <div className="ml-2.5 flex flex-col gap-1.5 rounded-2xl border border-border bg-bg-elevated/95 backdrop-blur-xl p-2 shadow-2xl">
              {NAV_ITEMS.map(item => {
                const Icon = item.icon
                const active = isActive(location.pathname, item.to)
                const hovered = hoveredItem === item.to

                return (
                  <motion.button
                    key={item.to}
                    type="button"
                    onClick={() => handleItemClick(item.to)}
                    onMouseEnter={() => handleItemHover(item.to)}
                    className={cn(
                      'relative flex items-center justify-center w-10 h-10 rounded-xl',
                      'transition-colors duration-150',
                      active
                        ? 'bg-accent text-white'
                        : hovered
                          ? 'bg-accent/15 text-accent'
                          : 'text-text-secondary hover:text-text',
                    )}
                    whileHover={{ scale: 1.12 }}
                    whileTap={{ scale: 0.95 }}
                    transition={{ type: 'spring', stiffness: 500, damping: 25 }}
                  >
                    <Icon size={18} />

                    {/* Tooltip */}
                    <AnimatePresence>
                      {hovered && !active && (
                        <motion.span
                          className="absolute left-full ml-3 px-2.5 py-1 rounded-lg bg-bg-elevated border border-border text-xs font-medium text-text whitespace-nowrap shadow-lg"
                          initial={{ opacity: 0, x: -6 }}
                          animate={{ opacity: 1, x: 0 }}
                          exit={{ opacity: 0, x: -6 }}
                          transition={{ duration: 0.12 }}
                        >
                          {item.label}
                        </motion.span>
                      )}
                    </AnimatePresence>
                  </motion.button>
                )
              })}
            </div>
          </motion.div>
        )}
      </AnimatePresence>
    </>
  )
}
