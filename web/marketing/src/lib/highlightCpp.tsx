// Small, dependency-free token highlighter shared by every code panel that
// shows a C++ snippet (Hero, GettingStarted, the API page) — not a general
// syntax highlighter, just enough for the sampled snippets to read as real
// code.

export function highlightCpp(source: string) {
  const lines = source.split("\n");
  return lines.map((line, i) => {
    const parts: Array<{ text: string; cls?: string }> = [];
    const re =
      /(\/\/.*$)|("[^"]*")|\b(struct|auto|static|constexpr|std|using|namespace)\b|\b([A-Z][A-Za-z0-9_]*)\b/g;
    let last = 0;
    let m: RegExpExecArray | null;
    while ((m = re.exec(line))) {
      if (m.index > last) parts.push({ text: line.slice(last, m.index) });
      if (m[1]) parts.push({ text: m[1], cls: "tok-com" });
      else if (m[2]) parts.push({ text: m[2], cls: "tok-str" });
      else if (m[3]) parts.push({ text: m[3], cls: "tok-kw" });
      else if (m[4]) parts.push({ text: m[4], cls: "tok-type" });
      last = re.lastIndex;
    }
    if (last < line.length) parts.push({ text: line.slice(last) });
    return (
      <div key={i}>
        {parts.map((p, j) =>
          p.cls ? (
            <span key={j} className={p.cls}>
              {p.text}
            </span>
          ) : (
            <span key={j}>{p.text}</span>
          ),
        )}
        {line.length === 0 ? " " : null}
      </div>
    );
  });
}
