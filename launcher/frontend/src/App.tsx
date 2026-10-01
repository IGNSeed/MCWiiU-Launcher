import { useCallback, useEffect, useState } from 'react';
import { getAppInfo, getRuntimeState, openDevTools } from './bridge';
import type { AppInfo } from './bridge';
import { strings } from './strings';

export default function App() {
  const [info, setInfo] = useState<AppInfo>();
  const [busy, setBusy] = useState(true);
  const [error, setError] = useState('');
  const [toolsError, setToolsError] = useState('');

  const refresh = useCallback(async () => {
    setBusy(true);
    setError('');
    try {
      const [app, state] = await Promise.all([getAppInfo(), getRuntimeState()]);
      setInfo({ ...app, runtimeState: state });
    } catch {
      setInfo(undefined);
      setError(strings.bridgeUnavailable);
    } finally {
      setBusy(false);
    }
  }, []);

  useEffect(() => { void refresh(); }, [refresh]);

  const showDevTools = async () => {
    setToolsError('');
    try { await openDevTools(); }
    catch { setToolsError(strings.toolsUnavailable); }
  };

  return (
    <div className="shell">
      <aside className="sidebar">
        <div className="brand"><span className="brand-mark" aria-hidden="true">M</span><span>MCWiiU<span className="brand-subtitle">Launcher</span></span></div>
        <nav aria-label="Main navigation">
          {strings.navigation.map((page, index) => (
            <button key={page} className={index === 0 ? 'nav-item selected' : 'nav-item'}
              aria-current={index === 0 ? 'page' : undefined} disabled={index !== 0}
              title={index !== 0 ? strings.upcoming : undefined}>
              <span className="nav-marker" aria-hidden="true" />{page}
            </button>
          ))}
        </nav>
        <div className="sidebar-footer">{strings.platform}<br /><span>{strings.architecture}</span></div>
      </aside>
      <main>
        <header><span>{strings.edition}</span><span className="version-pill">{strings.supported}</span></header>
        <section className="hero">
          <div className="eyebrow">{strings.product}</div>
          <h1>{strings.welcome}</h1>
          <p>{strings.introduction}</p>
          <div className="region-label">{strings.regions}</div>
          <div className="landscape" aria-hidden="true"><i /><i /><i /><i /><i /><i /></div>
        </section>
        <section className="status-card" aria-labelledby="foundation-title">
          <div className="status-top"><div><div className="eyebrow">{info?.architecture ?? strings.architecture}</div><h2 id="foundation-title">{strings.foundation}</h2></div><span className="status-dot" data-connected={!!info} /></div>
          <dl><div><dt>{strings.runtime}</dt><dd id="runtime-state">{info?.runtimeState ?? strings.connecting}</dd></div></dl>
          <p className="status-note" role="status">{error || (info ? strings.connected : strings.connecting)}</p>
          <p className="muted">{strings.pending}</p>
          <button className="primary-button" onClick={() => void refresh()} disabled={busy}>{busy ? strings.refreshing : strings.refresh}</button>
          {info?.development && <div className="dev-row"><span>{strings.development} · {info.configuration}</span><button className="text-button" onClick={() => void showDevTools()}>{strings.developerTools}</button>{toolsError && <p role="alert">{toolsError}</p>}</div>}
        </section>
        <footer><h3>{strings.futureTitle}</h3><p>{strings.futureDescription}</p></footer>
      </main>
    </div>
  );
}
