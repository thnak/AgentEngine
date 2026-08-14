import { createContext, useContext, useEffect, useMemo, useState, type PropsWithChildren } from "react";

export type Lang = "en" | "vi";

const STORAGE_KEY = "ae-lang";

function readStoredLang(): Lang {
  if (typeof window === "undefined") return "en";
  const stored = window.localStorage.getItem(STORAGE_KEY);
  return stored === "vi" ? "vi" : "en";
}

interface LanguageContextValue {
  lang: Lang;
  setLang: (lang: Lang) => void;
  toggle: () => void;
}

const LanguageContext = createContext<LanguageContextValue | null>(null);

// Wraps every page root (App.tsx, ApiApp.tsx, each api/*DetailApp.tsx) — this is a static
// multi-page site with one React root per HTML entry, so the provider is mounted once per
// page rather than once globally. Persists to localStorage so a language choice survives
// navigating between pages (each of which is a separate document load, not a client route).
export function LanguageProvider({ children }: PropsWithChildren) {
  const [lang, setLang] = useState<Lang>(readStoredLang);

  useEffect(() => {
    window.localStorage.setItem(STORAGE_KEY, lang);
    document.documentElement.lang = lang;
  }, [lang]);

  const value = useMemo<LanguageContextValue>(
    () => ({
      lang,
      setLang,
      toggle: () => setLang((l) => (l === "en" ? "vi" : "en")),
    }),
    [lang],
  );

  return <LanguageContext.Provider value={value}>{children}</LanguageContext.Provider>;
}

export function useLang(): LanguageContextValue {
  const ctx = useContext(LanguageContext);
  if (!ctx) throw new Error("useLang() called outside LanguageProvider");
  return ctx;
}
