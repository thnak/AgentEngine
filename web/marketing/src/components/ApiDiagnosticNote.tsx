import type { ReactNode } from "react";

/** The site's citation channel: a compiler-diagnostic-styled aside that carries an RFC/ADR/file/
 * test reference OUT of prose. Drop one after any paragraph that used to fold its proof into a
 * parenthetical — the paragraph states its claim once, plainly; this is where a reader goes to
 * audit it. `status` picks the left-rule color and the implicit "note:" kind label; pass `kind`
 * to override the label (e.g. "see also", "caveat"). */
export function ApiDiagnosticNote({
  status = "real",
  kind,
  children,
}: {
  status?: "real" | "design" | "stub";
  kind?: string;
  children: ReactNode;
}) {
  const label = kind ?? (status === "design" ? "spec" : status === "stub" ? "caveat" : "note");
  return (
    <p className={`diagnostic-note is-${status}`}>
      <span className="kind">{label}:</span>
      <span>{children}</span>
    </p>
  );
}
