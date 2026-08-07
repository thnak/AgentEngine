import type { ReactNode } from "react";

export function ApiTable({
  columns,
  rows,
  templateColumns,
}: {
  columns: string[];
  rows: ReactNode[][];
  templateColumns?: string;
}) {
  const gridTemplateColumns = templateColumns ?? columns.map(() => "1fr").join(" ");
  return (
    <div className="api-table glass">
      <div className="api-table-row api-table-head" style={{ gridTemplateColumns }}>
        {columns.map((c) => (
          <span key={c}>{c}</span>
        ))}
      </div>
      {rows.map((row, i) => (
        <div className="api-table-row" style={{ gridTemplateColumns }} key={i}>
          {row.map((cell, j) => (
            <span key={j}>{cell}</span>
          ))}
        </div>
      ))}
    </div>
  );
}
