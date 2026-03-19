import { useState } from 'react'
import { Sun, Moon } from 'lucide-react'
import { getTheme, toggleTheme } from '../../theme'

export function TopBar() {
  const [theme, setThemeState] = useState(getTheme)

  const handleToggle = () => {
    const next = toggleTheme()
    setThemeState(next)
  }

  return (
    <header className="h-12 flex items-center justify-between px-4 border-b border-border bg-bg-surface shrink-0">
      <span className="text-accent font-bold tracking-tight text-lg">orangutan</span>
      <button
        onClick={handleToggle}
        className="p-1.5 rounded hover:bg-bg-elevated text-text-muted hover:text-text transition-colors"
        aria-label="Toggle theme"
      >
        {theme === 'dark' ? <Sun size={18} /> : <Moon size={18} />}
      </button>
    </header>
  )
}
