// SPDX-License-Identifier: GPL-2.0-or-later
// Reports windows to ahkd. Loaded by ahkd through org.kde.kwin.Scripting, unloaded when it exits.
const S = "io.github.ahklinux.Bridge", SEP = "\u001f";
function call(method, f) { callDBus(S, "/", S, method, f.map(x => String(x).split(SEP).join(" ")).join(SEP)); }
function wanted(w) { return w && !w.deleted && (w.normalWindow || w.dialog || w.utility || w.splash); }
function put(w) {
    if (!wanted(w)) return;
    const g = w.frameGeometry, c = w.clientGeometry;
    call("Window", [w.internalId, w.caption, w.resourceClass, w.pid, w.minimized ? 1 : 0,
        ...[g.x, g.y, g.width, g.height, c.x, c.y, c.width, c.height].map(Math.round)]);
}
function active() {
    const w = workspace.activeWindow, v = workspace.virtualScreenSize;
    call("Active", [wanted(w) ? w.internalId : "", v.width, v.height]);
}
function watch(w) {
    if (!w) return;
    put(w);
    const again = () => put(w);
    for (const s of ["captionChanged", "frameGeometryChanged", "minimizedChanged", "windowClassChanged"]) if (w[s]) w[s].connect(again);
}
workspace.windowList().forEach(watch);
workspace.windowAdded.connect(watch);
workspace.windowRemoved.connect(w => call("Gone", [w.internalId]));
workspace.windowActivated.connect(w => { put(w); active(); });
active();
