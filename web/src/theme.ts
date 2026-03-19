export type Theme = 'dark' | 'light'

export function getTheme(): Theme {
  return (localStorage.getItem('theme') as Theme) ?? 'dark'
}

export function setTheme(theme: Theme) {
  localStorage.setItem('theme', theme)
  document.documentElement.setAttribute('data-theme', theme)
}

export function toggleTheme(): Theme {
  const next = getTheme() === 'dark' ? 'light' : 'dark'
  setTheme(next)
  return next
}

// Initialize on load
setTheme(getTheme())
