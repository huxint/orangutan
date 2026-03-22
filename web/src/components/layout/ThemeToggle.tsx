import { useState } from "react";
import { Sun, Moon } from "lucide-react";
import { getTheme, toggleTheme } from "../../theme";

export function ThemeToggle() {
  const [theme, setThemeState] = useState(getTheme);

  const handle = () => {
    const next = toggleTheme();
    setThemeState(next);
  };

  return (
    <button
      onClick={handle}
      className="fixed top-3 right-3 z-30 p-2 rounded-xl
        border border-border bg-bg-surface/80 backdrop-blur-md
        text-text-secondary hover:text-text hover:border-accent/20
        transition-all duration-200"
      aria-label="Toggle theme"
    >
      {theme === "dark" ? <Sun size={16} /> : <Moon size={16} />}
    </button>
  );
}
