import { useLang } from "../i18n/LanguageContext";
import { ui } from "../i18n/ui";
import {
  quickPickRows,
  useCaseHowToPick,
  useCaseGroups,
  type UseCaseRow,
} from "../data/useCaseContent";
import { ApiTable } from "./ApiTable";
import { RevealGroup, RevealItem } from "./Reveal";

function StatusBadge({ status }: { status: UseCaseRow["status"] }) {
  const { lang } = useLang();
  const tu = ui[lang];
  return (
    <span className={`status-badge status-${status}`}>
      {status === "real" ? tu.statusRealTested : tu.statusDesignedNotBuilt}
    </span>
  );
}

// One catalog row: the feature (linked to its own API page), the use case to
// steal, the runnable example that already proves it, and the entry points to
// implement against. The badge carries the real-vs-design honesty.
function UseCaseCard({ row }: { row: UseCaseRow }) {
  const { lang } = useLang();
  return (
    <article className="doc-entry" id={row.id}>
      <div className="doc-entry-head">
        <a className="api-tag" href={row.featureHref}>
          {row.feature[lang]}
        </a>
        <StatusBadge status={row.status} />
      </div>
      <p>{row.idea[lang]}</p>
      <div className="use-case-meta">
        <span className="use-case-meta-label">Start from</span>
        <a className="use-case-link" href={row.exampleHref} target="_blank" rel="noreferrer">
          {row.example[lang]}
        </a>
      </div>
      <div className="use-case-meta">
        <span className="use-case-meta-label">Implement against</span>
        <code className="use-case-entry">{row.entry[lang]}</code>
      </div>
    </article>
  );
}

// The quick-pick table: "I want to …" -> the feature to reach for -> the
// example to run. Every cell is a link into the real artifact.
function QuickPickTable() {
  const { lang } = useLang();
  const rows = quickPickRows.map((r) => [
    <span key="goal">{r.goal[lang]}</span>,
    <a key="pick" href={r.pickHref} style={{ textDecoration: "underline" }}>
      {r.pick[lang]}
    </a>,
    <a key="start" href={r.startHref} target="_blank" rel="noreferrer" className="api-cite">
      {r.start}
    </a>,
  ]);
  return (
    <ApiTable
      columns={["I want to…", "Pick", "Start from"]}
      rows={rows}
      templateColumns="1.4fr 1fr 1.6fr"
    />
  );
}

export function ApiUseCasesReference() {
  const { lang } = useLang();
  const how = useCaseHowToPick[lang];
  return (
    <>
      <section className="section" id="how-to-pick">
        <div className="container">
          <div className="section-head">
            <span className="eyebrow">{how.eyebrow}</span>
            <h2>{how.heading}</h2>
            <p>{how.body}</p>
          </div>
        </div>
      </section>

      <section className="section" id="quick-pick">
        <div className="container">
          <div className="section-head">
            <span className="eyebrow">The fastest path</span>
            <h2>Quick pick: by goal</h2>
            <p>
              Know what you want to build, not which feature it maps to? Read down
              the left column, take the example in the right, and change it.
            </p>
          </div>
          <QuickPickTable />
        </div>
      </section>

      {useCaseGroups.map((group) => (
        <section className="section" id={group.id} key={group.id}>
          <div className="container">
            <div className="section-head">
              <span className="eyebrow">{group.eyebrow[lang]}</span>
              <h2>{group.label[lang]}</h2>
              <p>{group.description[lang]}</p>
            </div>
            <RevealGroup className="doc-entries">
              {group.rows.map((row) => (
                <RevealItem key={row.id}>
                  <UseCaseCard row={row} />
                </RevealItem>
              ))}
            </RevealGroup>
          </div>
        </section>
      ))}
    </>
  );
}
