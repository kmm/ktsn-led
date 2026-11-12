import { useMemo, useState } from 'react';
import { CATEGORIES, ENTRIES, type Entry, ENTRY_BY_NAME, signature, type Topic, TOPICS } from '../reference/entries';

interface Props {
  /** Entry or topic to show (set from the editor: hover link, F1). */
  selected: string | null;
  onSelect(id: string | null): void;
  /** Load an example into the editor and run it in the preview. */
  onTry(src: string): void;
}

function Paragraphs({ text }: { text: string }) {
  return <>{text.split('\n\n').map((p, i) => <p key={i} className="ref-p">{p}</p>)}</>;
}

/** An example program with a button that loads it into the editor and runs it in the preview. */
function Example({ src, onTry }: { src: string; onTry(src: string): void }) {
  return (
    <div className="ref-example">
      <pre className="code">{src}</pre>
      <button onClick={() => onTry(src)} title="Load into the editor and run it in the preview (not sent to a board)">▶ Try it</button>
    </div>
  );
}

function EntryView({ e, onTry, onSelect }: { e: Entry; onTry(s: string): void; onSelect(id: string): void }) {
  return (
    <article className="ref-entry">
      <h2><code>{e.name}</code> <span className="muted small">{e.category}</span></h2>
      <pre className="sig">{signature(e)}</pre>
      <p>{e.summary}</p>
      {e.args.length > 0 && (
        <table className="ref-args">
          <tbody>
            {e.args.map((a) => (
              <tr key={a.name}>
                <td><code>{a.optional ? `[${a.name}]` : a.name}{a.many && a.many > 1 ? '…' : ''}</code></td>
                <td className="muted">{a.type}</td>
                <td>{a.desc}{a.def && <span className="muted"> (default {a.def})</span>}{a.many && a.many > 1 && <span className="muted"> (up to {a.many})</span>}</td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
      <p><span className="muted">Returns</span> {e.returns}</p>
      {e.details && <Paragraphs text={e.details} />}
      <h3>Example</h3>
      <Example src={e.example} onTry={onTry} />
      <p className="small muted">See also: {TOPICS.filter((t) => t.id === 'ranges' || t.id === 'values').map((t, i) => (
        <span key={t.id}>{i ? ', ' : ''}<a href="#" onClick={(ev) => { ev.preventDefault(); onSelect(`topic:${t.id}`); }}>{t.title}</a></span>
      ))}</p>
    </article>
  );
}

function TopicView({ t, onTry }: { t: Topic; onTry(s: string): void }) {
  return (
    <article className="ref-entry">
      <h2>{t.title}</h2>
      <Paragraphs text={t.body} />
      {t.example && <><h3>Example</h3><Example src={t.example} onTry={onTry} /></>}
    </article>
  );
}

export function ReferencePanel({ selected, onSelect, onTry }: Props) {
  const [query, setQuery] = useState('');
  const q = query.trim().toLowerCase();
  const matches = useMemo(() => ENTRIES.filter((e) => !q || e.name.includes(q) || e.summary.toLowerCase().includes(q) || e.category.toLowerCase().includes(q)), [q]);

  if (selected) {
    const topic = selected.startsWith('topic:') ? TOPICS.find((t) => `topic:${t.id}` === selected) : undefined;
    const entry = topic ? undefined : ENTRY_BY_NAME.get(selected);
    return (
      <div className="reference">
        <button className="link" onClick={() => onSelect(null)}>← All functions</button>
        {topic ? <TopicView t={topic} onTry={onTry} /> : entry ? <EntryView e={entry} onTry={onTry} onSelect={onSelect} /> : <p className="muted">No entry for "{selected}".</p>}
      </div>
    );
  }

  return (
    <div className="reference">
      <input className="search" type="search" placeholder="Search functions…" value={query} onChange={(e) => setQuery(e.target.value)} autoFocus />
      {!q && (
        <section>
          <h3>Guide</h3>
          <ul className="ref-list">
            {TOPICS.map((t) => <li key={t.id}><a href="#" onClick={(ev) => { ev.preventDefault(); onSelect(`topic:${t.id}`); }}>{t.title}</a></li>)}
          </ul>
        </section>
      )}
      {CATEGORIES.map((c) => {
        const list = matches.filter((e) => e.category === c);
        if (!list.length) return null;
        return (
          <section key={c}>
            <h3>{c}</h3>
            <ul className="ref-list">
              {list.map((e) => (
                <li key={e.name}>
                  <a href="#" onClick={(ev) => { ev.preventDefault(); onSelect(e.name); }}><code>{e.name}</code></a>
                  <span className="muted"> {e.summary}</span>
                </li>
              ))}
            </ul>
          </section>
        );
      })}
      {q && !matches.length && <p className="muted">Nothing matches "{query}".</p>}
    </div>
  );
}
