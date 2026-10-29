# Spec sheet: `session` group (MuseScore Studio 4.7.6)

Scout: `api-scout`, task P0.S-session. Source: branch `agent/4.7` at `v4.7.6` (working tree,
identity patch applied). All paths are relative to the repo root.

Commands: hello, instances_list, scores_list, score_open, undo, redo, save, backup_snapshot.

Abbreviations:
`pac.cpp` = `src/project/internal/projectactionscontroller.cpp`,
`np.cpp` = `src/project/internal/notationproject.cpp`,
`pas.cpp` = `src/project/internal/projectautosaver.cpp`,
`pcfg.cpp` = `src/project/internal/projectconfiguration.cpp`,
`mpp.cpp` = `src/framework/multiwindows/internal/multiprocess/multiprocessprovider.cpp`,
`ipc/` = `src/framework/multiwindows/internal/multiprocess/ipc/`,
`nus.cpp` = `src/notation/internal/notationundostack.cpp`,
`ni.cpp` = `src/notation/internal/notationinteraction.cpp`,
`nac.cpp` = `src/notationscene/internal/notationactioncontroller.cpp`,
`cmd.cpp` = `src/engraving/editing/cmd.cpp`, `undo.cpp`/`undo.h` = `src/engraving/editing/undo.{cpp,h}`,
`textedit.cpp` = `src/engraving/editing/textedit.cpp`,
`writer.cpp` = `src/engraving/rw/write/writer.cpp`.

Shared facts from other sheets are cited, not repeated: `notes-G1` (lock semantics, empty
macro discarded, confirm `hasActiveCommand` after `prepareChanges`), `notes-G6` (mode
preconditions). Error codes are proposed names for the conductor's error table.

URI note: COMMANDS.md says `command://<area>/<name>`. These commands are not notation edits;
proposed areas are `app` (hello, instances_list), `project` (scores_list, score_open, save,
backup_snapshot) and `notation` (undo, redo, matching the UI action codes
`action://notation/undo|redo`, `nac.cpp:52-53`). Final naming is the conductor's call.

---

## 0. Group-wide findings (S1–S12)

**S1. One score per process, and the process owns it.** In 4.7.6 the multi-window module uses
`MultiProcessProvider` because `MUSE_MULTICONTEXT_WIP` is OFF by default
(`src/framework/cmake/MuseDeclareOptions.cmake:103`, `src/framework/multiwindows/multiwindowsmodule.cpp:33-37`, `:49-56`).
Each process has one `IGlobalContext` with at most one current project
(`src/context/iglobalcontext.h:39-48`). `ProjectActionsController::openProject(path)` opens a
file in the current process only if no project is open there; otherwise it starts a new process
(`pac.cpp:265-277` → `mpp.cpp:258-304`, `QProcess::startDetached(applicationFilePath, args)` at
`mpp.cpp:266-267`). A process whose score is closed while another process has no score quits
(`pac.cpp:70-80`). So "instance" = process, and an instance holds 0 or 1 project over its life,
which can change (close → Home page, then open another in the same process, `pac.cpp:296-297`).

**S2. The IPC channel cannot carry agent traffic.** The channel is a `QLocalServer`/`QLocalSocket`
star: the first process to start becomes the server (`ipc/ipcchannel.cpp:150-171`), others
re-elect on disconnect after a random 1–100 ms delay (`ipc/ipcchannel.cpp:173-183`). The
server name is `musescore-agent-ipc-4` after the identity patch (`ipc/ipc.h:37-41`), so agent
windows never join the official app's group. Limits that rule it out for request forwarding:
- Fixed method set, dispatched in `MultiProcessProvider::onMsg` (`mpp.cpp:86-173`); unknown
  methods are silently ignored (the if/else chain has no default). `m_ipcChannel` is private
  (`multiprocessprovider.h:108`). Adding a method means editing an upstream file.
- 4096-byte package limit; larger messages are dropped with an assert (`ipc/ipc.h:58`,
  `ipc/ipc.cpp:86-91`, `:116-120`).
- Synchronous requests wait in a nested `QEventLoop` for at most 500 ms
  (`ipc/ipcchannel.cpp:100-128`, `ipc/ipcloop.cpp:36-45`, `ipcloop.h:41`) and stop at the
  first positive answer (`ipc/ipcchannel.cpp:115-119`): it is a yes/no poll, not an enumeration.
- `InstanceMeta` carries only a UUID and an `isServer` flag (`imultiprocessprovider.h:32-36`), and
  `instances()` never fills `isServer` (`mpp.cpp:442-453`). The UUID is created per process
  (`ipc/ipcsocket.cpp:38-44`). No instance can learn another's project path except by asking
  "is path P open?" (`mpp.cpp:93-102`, `:175-194`).
- The agent must not open its own `IpcChannel` on the same server name: every connected socket
  counts as a window (`mpp.cpp:427-430`), and `windowCount()` drives startup, session restore and
  quit decisions (`src/appshell/internal/startupscenario.cpp:109`, `:154`, `:304`;
  `src/appshell/internal/sessionsmanager.cpp:46`; `src/appshell/internal/applicationactioncontroller.cpp:262-281`).
  An extra socket that never answers `PROJECT_IS_OPENED` would also make every
  `isProjectAlreadyOpened` call wait the full 500 ms (`ipc/ipcchannel.cpp:102-106`, `:121-123`).
Forwarding therefore needs the agent's own channel (per-process HTTP port + discovery record,
as in assessment §"multi-instance").

**S3. Nested event loops on the open path make the GUI thread re-entrant.**
`isProjectAlreadyOpened` spins `IpcLoop` up to 500 ms (`mpp.cpp:175-194`, `ipc/ipcchannel.cpp:128`);
`openNewWindow` spins a `QEventLoop` 50 × 100 ms waiting for the child to register
(`mpp.cpp:275-298`). While these run, Qt delivers other events: another HTTP request, a user
click, the autosave timer (`pas.cpp:34-38`). `ProjectActionsController` defends only itself
with `m_isProjectProcessing` (returns `InternalError`, `pac.cpp:238-245`). The agent server must
hold its own "request in progress" guard and refuse (E_BUSY) re-entrant requests.

**S4. A user drag holds an open macro across event-loop iterations.** `startDrag` calls
`startEdit` → `prepareChanges` (`ni.cpp:1205`, `ni.cpp:314-319`) and the macro is closed only in
`endDrag` → `apply()` (`ni.cpp:1361-1363`, `ni.cpp:321-330`). An agent request arriving between
two mouse-move events finds `undoStack()->hasActiveCommand()` true (`undo.h:272`). In that state
`prepareChanges`/`startCmd` silently do nothing (`cmd.cpp:354-357`), so an agent write would
land in the user's drag macro and the agent's commit would close it early (`cmd.cpp:406-447`).
For this group: undo, redo, save and backup_snapshot must all refuse with E_BUSY when
`hasActiveCommand()` or `isLocked()` is true (concrete instance of notes-G1).

**S5. Undo/redo check neither the lock nor an active macro.** `NotationUndoStack::undo/redo`
call `Score::undoRedo` unconditionally (`nus.cpp:47-80`); `Score::undoRedo` checks only
`readOnly()` (`cmd.cpp:372-376`); `UndoStack::undo/redo` check only the text-edit start index
and `canRedo` (`undo.cpp:465-493`). Undo while a macro is open (agent batch, plugin
`startCmd`, user drag) reverts the *previous* macro while the open one keeps collecting
children; the later `endMacro` then deletes the undone macro as "redo stack" (`undo.cpp:426-432`).
`MasterScore::setReadOnly` (`src/engraving/dom/masterscore.h:99`) has no caller in the app
(`git grep setReadOnly` finds only unrelated palette widgets), so `readOnly()` never guards anything.

**S6. Text editing absorbs any undo entries pushed while it is active.** `TextBase::startEdit`
records `startUndoIdx = currentIndex()` (`textedit.cpp:117`); `endEdit` merges *every* macro
from that index into one (`textedit.cpp:162-165`), filters text-edit children
(`undo.cpp:225-236`), then `reopen()`s it and commits it again (`textedit.cpp:233-259`). An agent
transaction committed while the user is typing would lose its own undo entry and name and
become part of the user's text edit. Text editing spans event-loop iterations
(`ni.cpp:4354`, `ni.cpp:4589`). Refuse writes and undo/redo while
`INotationInteraction::isTextEditingStarted()` (`src/notation/inotationinteraction.h:152`) → E_BUSY.

**S7. Any score write can open and roll back a command.** `Writer::write` (used by save,
autosave, `writeToDevice`, `MscNotationWriter` and every mscz/mscx export) does, when
`Sid::createMultiMeasureRests` is on and a part is hidden: `startCmd("Unhide instruments for
save")`, `undoChangeProperty(VISIBLE, true)`, `doLayout()`, write, `endCmd(true)`
(`writer.cpp:96-113`, `writer.cpp:273-275`). If a macro is already active, the `startCmd` is a
no-op (`cmd.cpp:354-357`) and the `endCmd(true)` rolls back and closes the *outer* macro
(`cmd.cpp:418-437`). If the stack is locked, both are no-ops (`cmd.cpp:346-348`, `:408-410`) and
the VISIBLE change stays in the outer macro. Therefore save and backup_snapshot (and the read
group's score_export) must run only with no active macro and the stack unlocked, never inside a
batch.

**S8. Every full write mutates in-memory state that is not on the undo stack.**
- `m_mscoreVersion`, `m_mscoreRevision`, `m_mscVersion` are overwritten with the running app's
  values (`writer.cpp:72-77`). After the first save, backup or export of an older 4.x file,
  `IMasterNotation::mscVersion()` (`src/notation/internal/masternotation.cpp:118-125`) reports 470,
  not the file's format. `Box` auto-size depends on it (`src/engraving/dom/box.cpp:197`).
- Items without an EID get one assigned (`src/engraving/rw/write/twrite.cpp:495-511`, assignment
  at `:501-503`).
- Excerpt file names are renumbered (`src/engraving/rw/mscsaver.cpp:94`).
- With `createThumbnail`, `Score::createThumbnail` switches to page mode, prints, switches back
  and runs a full `doLayout()` (`src/engraving/dom/scorefile.cpp:74-104`).
None of these mark the project dirty (dirty state comes only from the undo stack and the
listeners in `np.cpp:1028-1079`).

**S9. `stateVersion` = undo index is ambiguous.** `currentStateIndex()` is
`UndoStack::m_currentIndex` (`nus.cpp:223-230`, `undo.h:286`). Undo decrements it
(`undo.cpp:476-480`); a new commit after an undo discards the redo stack and increments it
again (`undo.cpp:426-435`), so "user undid A then did B" returns the same index as before. The
stack keeps a unique per-macro state number (`m_stateList`, `m_nextState`, `undo.h:314-315`,
`undo.cpp:434`) but it is private and not exposed through `INotationUndoStack`
(`src/notation/internal/inotationundostack.h:36-70`). The assessment's stateVersion definition
(assessment.md line 130) needs another component (see Open questions).

**S10. The clean state is never moved on save.** `UndoStack::m_cleanState` is set to 0 in the
constructor and never written again (`undo.cpp:256-263`; `git grep m_cleanState` finds no other
write), and `isClean()` compares the current state with it (`undo.h:283`). `NotationProject`
recomputes `needSave` on every `ScoreChanges` as `!(isStackClean && !m_hasNonUndoStackChanges)`
(`np.cpp:1035-1043`). Consequence: open (state 0) → edit → save (`needSave=false`, `np.cpp:1081-1093`)
→ undo back to state 0 → `needSave=false`, although the file on disk contains the undone edit.
The agent should report dirty state from `INotationProject::needSave()`
(`src/project/inotationproject.h:65`) as the UI does, and must not infer "file matches memory"
from it.

**S11. Dialog sources on the session paths.** All are in the controller or deeper, not in
`INotationProject::save/load` themselves except migration and importers:
- open: re-entrancy guard (`pac.cpp:238-240`), cloud login (`pac.cpp:280-294`, `:488-491`),
  load-error dialogs (`pac.cpp:1769-1873`), migration dialog inside `NotationProject::doLoad`
  for files older than 4.0 (`np.cpp:188-196` → `src/project/internal/projectmigrator.cpp:65-99`,
  `openSync` at `:121`), MusicXML import-error question (`src/importexport/musicxml/internal/import/importmusicxml.cpp:62-76`, `:119`),
  MEI warning (`src/importexport/mei/internal/meireader.cpp:76-91`), MuseSounds update / sampler
  status one second after open (`pac.cpp:422-430`), extension point `onPostProjectOpened`
  (`pac.cpp:420`).
- save: Save-As/location dialogs for new or imported scores (`pac.cpp:829-848`,
  `src/project/internal/opensaveprojectscenario.cpp:41`, `:122`, `:158`), corrupted-score
  question (`pac.cpp:968-976`, `:1513-1528`, `:1589-1625`), save failure warning
  (`pac.cpp:1005-1009`, `:1663-1676`), corrupted-after-save retry dialog (`pac.cpp:1010-1025`,
  `:1678-1710`), cloud flows (`pac.cpp:1038-1130`), extension points `onPreProjectSave` /
  `onPostProjectSaved` (`pac.cpp:997-999`, `:1030-1032`), which run plugins synchronously
  (`src/framework/extensions/internal/extensionsprovider.cpp:226-243`).
`IInteractive::isCurrentUriDialog()` (`src/framework/global/iinteractive.h:252`) exists for an
E_MODAL_OPEN check; whether it reports the `warningSync`/`errorSync` boxes is an open question.

**S12. Data folders.** `GlobalConfiguration::userAppDataPath()` is
`QStandardPaths::AppLocalDataLocation` (`src/framework/global/internal/globalconfiguration.cpp:95-111`).
The identity patch sets the application name to `MuseScoreAgent`
(`src/app/main.cpp:119-121`) under organisation `MuseScore` (`src/app/main.cpp:123`), so the
agent build's data folder is separate from the official app's (DECISIONS: official folder is
`~/Library/Application Support/MuseScore/MuseScore4`). Discovery records, backups and temp files
must live under `userAppDataPath()`; MuseScore's own new-score autosave already lives there
(`pcfg.cpp:657-660`).

---

### hello  ·  command://app/hello

**UI entry point.** None; `hello` is the handshake of the MCP server being ported in P1.1/P1.2.
This section records what each process can report, with the interface that provides it.

| Field | Source |
|---|---|
| App name / title | `IApplication::name()/title()` (`src/framework/global/iapplication.h:55-56`); `BaseApplication::appName/appTitle` from `MUSE_APP_NAME` / `MUSE_APP_TITLE` (`src/framework/global/internal/baseapplication.cpp:40-60`); title is "MuseScore Agent" (`version.cmake:43-45`) |
| Version | `IApplication::version()/fullVersion()` (`iapplication.h:59-60`, `baseapplication.cpp:71-97`); 4.7.6 (`version.cmake:30-34`) |
| Build number, git revision | `IApplication::build()/revision()` (`iapplication.h:61-62`, `baseapplication.cpp:99-115`), from `MUSE_APP_BUILD_NUMBER` / `MUSE_APP_REVISION` (`SetupConfigure.cmake:308-309`, default build number 1 at `version.cmake:38-40`) |
| Unstable flag | `IApplication::unstable()` (`iapplication.h:58`); `version.cmake:50` sets ON, release mode turns it OFF (`SetupConfigure.cmake:65-69`) |
| Bundle id | `MUSE_APP_GUI_IDENTIFIER com.liamgreen.MuseScoreAgent` (`version.cmake:27-28`) |
| File format written | `Constants::MSC_VERSION = 470`, `"4.70"` (`src/engraving/types/constants.h:31-32`) |
| Instance id | `IMultiProcessProvider::selfID()` (`imultiprocessprovider.h:44`), a fresh UUID per process (`ipc/ipcsocket.cpp:38-44`) |
| IPC server role | `IMultiProcessProvider::isMainInstance()` (`mpp.cpp:437-440`); can change when the server process quits (`ipc/ipcchannel.cpp:173-183`) |
| Window count | `IMultiWindowsProvider::windowCount()` (`mpp.cpp:427-430`) |
| Process id | Not exposed by any MuseScore interface (`git grep applicationPid` finds no use outside thirdparty); the module must take it from Qt/POSIX |
| Score (0 or 1) | `IGlobalContext::currentProject()` (`iglobalcontext.h:40`); `path()`, `displayName()`, `isNewlyCreated()`, `isImported()`, `isCloudProject()`, `needSave()` (`inotationproject.h:41-65`); `IMasterNotation::mscVersion()` (`imasternotation.h:50`, but see S8: valid only before the first write); `excerpts()` and `hasParts()` (`imasternotation.h:54`, `:66`) |
| Current view | `IGlobalContext::currentNotation()` (`iglobalcontext.h:47`): the master or an open part; `INotation::name()` (`src/notation/inotation.h:63`) |
| Undo state | `INotationUndoStack::canUndo/canRedo/currentStateIndex/topMostUndoActionName/topMostRedoActionName` (`inotationundostack.h:41-64`) |
| Modes | `noteInput()->isNoteInputMode()` (`inotationinteraction.h:43`, `notationnoteinput.cpp:90-93`), `isTextEditingStarted()` (`:152`), `isDragStarted()` (`:97`) |

**Selection set-up.** None.

**Arguments.** `{}` (optionally `instance`, see instances_list).

**Validation before mutation.** None; read-only. Never touch `currentNotation` (setting it marks
an excerpt open and the project dirty: `src/context/internal/globalcontext.cpp:92-102`,
`np.cpp:1045-1049`).

**Post-condition.** None.

**Nested commands.** None.

**Linked parts / excerpts.** Report excerpts from the master notation, not from the current view.

**Crash and corruption risks.** `IMasterNotation::mscVersion()` changes after any write (S8);
capture the loaded format version when the project becomes current (`currentProjectChanged`,
`iglobalcontext.h:41`) if hello is to report it.

**Existing upstream tests.** None for `IApplication` or `MultiProcessProvider`; mock at
`src/framework/multiwindows/tests/mocks/multiwindowsprovidermock.h`.

**Scenarios.**
- SESSION-HELLO-01: no score open → hello lists app/version, `score: null`.
- SESSION-HELLO-02: score open → path, displayName, needSave=false, undo index 0.
- SESSION-HELLO-03: old 4.x file, backup_snapshot, hello → reported file format unchanged (S8).

**Open questions.** See Q1 (stateVersion), Q9 (process id of children).

---

### instances_list  ·  command://app/instances_list

**UI entry point.** None in the UI. The nearest upstream facility is the dev dialog
`muse://devtools/multiwindows/info` (`multiwindowsmodule.cpp:61-64`), which lists only the IPC
UUIDs (`src/framework/multiwindows/qml/Muse/MultiWindows/multiinstancesdevmodel.cpp:43`, `:62-64`).

**What exists in 4.7.6 (requirements for forwarding).**
- Process ↔ project: one project per process (S1). Which process holds a path can only be
  polled with `IMultiWindowsProvider::isProjectAlreadyOpened(path)` (`mpp.cpp:175-194`), which
  asks every other instance via `IProjectProvider::isProjectOpened` (`mpp.cpp:93-102`) — implemented
  by `ProjectActionsController` as exact `path_t` equality with the current project's path
  (`pac.cpp:654-667`, export at `src/project/projectmodule.cpp:87`). No enumeration exists (S2).
- Bringing a sibling's window to the front: `activateWindowWithProject(path)` broadcasts
  (`mpp.cpp:196-208`); the owner calls `mainWindow()->requestShowOnFront()` (`mpp.cpp:103-114`).
  On macOS the caller does not push itself back (`mpp.cpp:202-205`).
- Process lifetime: a process can quit when its score is closed (`pac.cpp:70-80`) or when any
  instance broadcasts quit (`mpp.cpp:161-167`, `:469-497`).
- New processes receive only the file path and optional display name as arguments
  (`pac.cpp:268-275`; parsed at `src/app/internal/commandlineparser.cpp:188`, `:556-557`); any
  agent option (port, token) given to the first process is not passed on by `openNewWindow`
  (`mpp.cpp:258-267`).

**Selection set-up.** None.

**Arguments.** `{}`. Every other command gains optional `instance: string` (the instance id
from the discovery record; UUID from `selfID()` is one candidate).

**Validation before mutation.** Read-only. Each record must be checked for liveness before it is
listed (process alive, port answers); a record whose process is gone is dropped → for forwarded
calls E_INSTANCE_GONE (assessment line 177).

**Post-condition.** n/a.

**Nested commands.** None. The forwarding call must not use a nested event loop on the GUI
thread while a transaction is open (S3).

**Linked parts / excerpts.** n/a.

**Crash and corruption risks.**
- Router hand-over: the assessment has the holder of the base port route for siblings. When that
  process quits (S1), nobody rebinds the base port unless siblings retry, the way `IpcChannel`
  re-elects a server (`ipc/ipcchannel.cpp:173-183`). Without it, Claude's configured port goes
  dead while other windows are still open.
- Re-entrancy while forwarding (S3).
- Do not join the IPC group with a second socket (S2).

**Existing upstream tests.** None (`src/framework/multiwindows/tests` holds only a mock).

**Scenarios.**
- SESSION-INST-01: two windows → both records listed with distinct ports and paths.
- SESSION-INST-02: close the second window's score (process quits, `pac.cpp:70-80`) → its record
  disappears; a forwarded call returns E_INSTANCE_GONE, not a hang.
- SESSION-INST-03: quit the base-port holder while another window stays open → base port served
  again within N seconds.
- SESSION-INST-04: save-as in a sibling → its record's path updates (`np.cpp:419-428` notifies
  `pathChanged`).

**Open questions.** Q2 (router takeover), Q3 (port base 2212 vs 2412), Q4 (record location),
Q9 (child process environment).

---

### scores_list  ·  command://project/scores_list

**UI entry point.** No single UI list. Per process: `IGlobalContext::currentProject()`
(`iglobalcontext.h:40`) and `currentNotation()` (`:47`), backed by `GlobalContext`
(`src/context/internal/globalcontext.cpp:36-103`). Across processes: the agent's discovery
records (instances_list).

**Selection set-up.** None.

**Arguments.** `{ "includeParts": boolean }` (default true).

Per entry: `instance`, `path` (`INotationProject::path()`), `displayName`, `needSave`,
`isNewlyCreated`, `isImported`, `isCloudProject`, `current: boolean` (this process), and when
`includeParts`: master + each excerpt with `name` (`iexcerptnotation.h:41`), `isOpen`
(`inotation.h:74`) and whether it is the current view.

**Validation before mutation.** Read-only. Path semantics: for a newly created score
`path()` is the score title, not a file (`np.cpp:1004-1016`); for imported files it is the
import source path (`np.cpp:339`, `:343-344`). Report both flags so callers do not treat
`path` as a saved `.mscz`.

**Post-condition.** n/a.

**Nested commands.** None.

**Linked parts / excerpts.** `IMasterNotation::excerpts()` (`imasternotation.h:54`). Do not call
`setCurrentNotation` or `setExcerptIsOpen` (`imasternotation.h:63`) to inspect parts: opening a
part marks the project dirty (`np.cpp:1045-1049`).

**Crash and corruption risks.** None identified on the read path.

**Existing upstream tests.** None.

**Scenarios.**
- SESSION-LIST-01: one window, one score with two parts → one entry, three notations.
- SESSION-LIST-02: two windows → two entries from two instances, one `current`.
- SESSION-LIST-03: new unsaved score → `isNewlyCreated: true`, path is the title.

**Open questions.** Q4.

---

### score_open  ·  command://project/score_open

**UI entry point.** File > Open → action `file-open` → `ProjectActionsController::openProject(ActionData)`
(`pac.cpp:68`, `:189-198`) → `openProject(const ProjectFile&)` (`pac.cpp:200-228`). Exposed as
`IProjectFilesController::openProject(const ProjectFile&)` (`src/project/iprojectfilescontroller.h:44`).
A null `ProjectFile` opens the file dialog (`pac.cpp:204-216`); a local-file URL goes straight to
`openProject(path)` (`pac.cpp:219-221`). The plugin API's `readScore` uses the same entry
(`src/project/internal/engravingpluginapihelper.cpp:52-62`).

`openProject(path)` steps (`pac.cpp:230-298`):
1. Re-entrancy guard → `Ret::Code::InternalError` (`:238-245`).
2. Absolute path (`:248-252`), no canonicalisation beyond `absoluteFilePath`.
3. Already current in this process → `doFinishOpenProject()` (just shows the notation page)
   (`:254-257`, `:418-450`).
4. Open in another process → `activateWindowWithProject` and return Ok (`:259-263`).
5. This process already has a project → `openNewWindow({path})`, return Ok regardless of
   whether the child started (`:265-277`; return value of `openNewWindow` ignored).
6. Cloud project → network/login paths (`:279-294`).
7. Otherwise `doOpenProject` (`:296-297`): `loadProject` (`:300-330`) → `loadWithFallback`
   (`:332-350`, on error asks via `shouldRetryLoadAfterError` `:1769-1793`) → recent files
   (`:363-366`) → `globalContext()->setCurrentProject` (`:368`) → `doFinishOpenProject`, which
   opens the notation page (`:433-449`, `:646-652`) and schedules the MuseSounds checks
   (`:422-430`).

How the user's view changes: case 3 switches this window to the notation page; case 4 raises
the sibling's window (`mpp.cpp:103-114`); case 5 starts a new process whose startup dispatches
`file-open` (`src/appshell/internal/startupscenario.cpp:213-218`, `:323-326`) and shows a
"new instance" splash (`src/app/internal/guiapp.cpp:130-145`); case 7 replaces the Home page in
this window with the score.

Format handling inside `load`:
- Version gate in `makeReader`: newer than 470 → `FileTooNew`, older than 114 → `FileTooOld`,
  300 → `FileOld300Format` (`src/engraving/rw/mscloader.cpp:55-72`). The controller turns these
  into dialogs (`pac.cpp:1776-1781`); `FileTooNew` retries only if `disableVersionChecking`
  (`pac.cpp:1781`, `pcfg.cpp:789`).
- Files before 4.0: `ProjectMigrator` asks with a modal (`projectmigrator.cpp:76-85`, `:121`) unless
  the stored option for that migration type has `isAskAgain == false`; the default is `true`
  (`src/project/types/projecttypes.h:64-74`, read at `pcfg.cpp:507-545`). Such files then load as
  "imported/newly created" (`np.cpp:134-140`).
- Non-MuseScore formats go through importers (`np.cpp:119-126`, `:273-347`), some with dialogs (S11).
- Autosave recovery is silent: if `<file>.autosave` exists, the autosave is loaded instead and the
  project marked unsaved (`pac.cpp:309-323`, `pas.cpp:83-87`, `:115-118`).

**Selection set-up.** None.

**Arguments.**
```json
{ "type": "object", "required": ["path"],
  "properties": {
    "path": { "type": "string", "description": "absolute path to .mscz or .mscx" },
    "activate": { "type": "boolean", "default": true },
    "waitMs": { "type": "integer", "minimum": 0, "maximum": 10000, "default": 5000 } } }
```
Output: `{ instance, path, opened: "already-current" | "activated-sibling" | "new-instance" | "this-instance" }`.

**Validation before mutation.**
- E_BUSY: agent request already in progress, or this process's text edit / drag / open macro
  (S3, S4, S6).
- E_MODAL_OPEN: modal open in this process (S11).
- E_FILE_NOT_FOUND / E_FILE_UNREADABLE: checked by the agent first; otherwise the controller
  shows `warnProjectCannotBeOpened` (`pac.cpp:1787-1789`, `:1851-1873`).
- E_UNSUPPORTED_FORMAT: suffix not `mscz`/`mscx` (`engraving::isMuseScoreFile`,
  `pac.cpp:175-187`). Imports open dialogs (S11) and produce "newly created" projects that
  cannot be saved in place (save section); refuse them in v1 unless Liam wants imports.
- E_CLOUD_PROJECT: `IProjectConfiguration::isCloudProject(path)` (`pac.cpp:280`).
- E_FILE_TOO_OLD / E_FILE_TOO_NEW / E_NEEDS_MIGRATION: pre-read the `museScore version` attribute
  (the version gate is `mscloader.cpp:55-72`). `IMscMetaReader::readMeta` does not return the
  file version (`src/project/internal/mscmetareader.cpp:277-300` reads the attribute only to pick
  a parse branch; `ProjectMeta::mscVersion` stays 0), so the agent needs its own header read
  (`MscReader` + root element). Refuse < 400 when a migration dialog would show, and > 470.
- Corrupted files cannot be detected without loading (`pac.cpp:1782-1786`); a failed load in
  this process would show a dialog. Open question Q6.

**Post-condition.** Case 3/7: `globalContext()->currentProject()->path() == absolute path`
(`pac.cpp:654-667`). Case 4: the sibling record with that path exists. Case 5: poll discovery
records for the path up to `waitMs`; if none appears → E_OPEN_TIMEOUT (the child may be showing a
dialog the agent cannot see).

**Nested commands.** Opening does not touch the current score's undo stack. `migrateProject`
runs `startCmd`/`endCmd` on the *new* score's own stack before it becomes current
(`projectmigrator.cpp:174`). Opening never runs inside an agent transaction. No
`MasterNotationParts::startGlobalEdit`-style lock/unlock on this path (the only other
`undoStack()->lock()` sites are new-score creation: `src/notation/internal/masternotation.cpp:257-270`,
`np.cpp:403-405`).

**Linked parts / excerpts.** n/a.

**Crash and corruption risks.**
- Re-entrancy during the IPC and `openNewWindow` loops (S3).
- Case 5 can fork processes repeatedly if called in a loop; the controller does not dedupe
  pending opens across processes (only `m_isProjectProcessing` in this process, `pac.cpp:238`).
- Path equality is literal (`pac.cpp:662`); the same file reached through a different path
  spelling would open twice (Q7).
- Plugin API caution: `readScore` with a score already open returns the *old* score, because
  `openProject` returns Ok after spawning a new process and the helper then reads
  `currentNotation()` of the calling process (`engravingpluginapihelper.cpp:56-60`,
  `pac.cpp:267-276`); the plugin even logs "Will open a score in a new window"
  (`src/engraving/api/v1/qmlpluginapi.cpp:297-303`).

**Existing upstream tests.** None for `ProjectActionsController` or `NotationProject::load`
(`src/project/tests` covers the meta reader, recent files, templates and convert-to-score only).
Mocks: `src/project/tests/mocks/notationprojectmock.h`, `projectconfigurationmock.h`,
`src/framework/multiwindows/tests/mocks/multiwindowsprovidermock.h`.

**Scenarios.**
- SESSION-OPEN-01: no score open → opens here, `opened: this-instance`, no dialog.
- SESSION-OPEN-02: same path again → `already-current`, undo index unchanged.
- SESSION-OPEN-03: score A open, open B → new instance appears in instances_list within waitMs.
- SESSION-OPEN-04: B already open in a sibling → `activated-sibling`, no new process.
- SESSION-OPEN-05: 3.x file → E_NEEDS_MIGRATION, no dialog, no process started.
- SESSION-OPEN-06: missing file → E_FILE_NOT_FOUND, no dialog.
- SESSION-OPEN-07: `.musicxml` → E_UNSUPPORTED_FORMAT (or import, per Liam).
- SESSION-OPEN-08: `<file>.autosave` present → opened content equals the autosave, needSave=true.

**Open questions.** Q5 (imports), Q6 (corrupted files), Q7 (path canonicalisation), Q8
(focus stealing on case 5).

---

### undo  ·  command://notation/undo

**UI entry point.** Edit > Undo, toolbar, Cmd+Z: `action://notation/undo` (`nac.cpp:52`) registered
at `nac.cpp:241` → overload `nac.cpp:2523-2527` → `nac.cpp:2464-2481`, which calls
`currentNotation()->interaction()->undo()` then `seekSelectedElement()` (`nac.cpp:2200-2213`).
App-level shortcut path: `action://undo` → `doGlobalUndo` dispatches the same code when a project
is open and focused (`src/appshell/internal/applicationuiactions.cpp:257-264`,
`src/appshell/internal/applicationactioncontroller.cpp:436-443`). Enablement:
`canReceiveAction` requires a master notation, not the review page, and `canUndo()` — note-input
and text-edit modes are *not* excluded (`nac.cpp:580-599`, `:2297-2300`).
`NotationInteraction::undo()` → `m_undoStack->undo(&m_editData)` (`ni.cpp:4643-4646`) →
`Score::undoRedo(true, ed)` (`nus.cpp:47-58`, `cmd.cpp:372-398`) → `UndoStack::undo`
(`undo.cpp:465-481`) → `UndoMacro::undo` (`undo.cpp:550-564`). Afterwards `NotationUndoStack`
notifies `notationChanged`, `undoRedoNotification` and `stackChanged` (`nus.cpp:55-57`);
`NotationInteraction` ends element editing on `undoRedoNotification` unless text editing
(`ni.cpp:249-253`). `ScoreChanges` are published (`cmd.cpp:396-397`) on the master score's
channel (`src/engraving/dom/score.cpp:6297`).

**Selection set-up.** None. Undo restores the selection and the whole `InputState` (including
`noteEntryMode`, `src/engraving/dom/input.h:115-116`) captured when the undone macro was opened
(`undo.cpp:544-548`, `:559-563`). An agent transaction opened while the user was in note input
will, when undone, put the score back into note-input state at that position
(`notationnoteinput.cpp:90-93` reads the flag from the score). Same as for any UI command.

**Arguments.**
```json
{ "type": "object", "properties": {
    "steps": { "type": "integer", "minimum": 1, "maximum": 50, "default": 1 },
    "expectStateVersion": { "description": "refuse unless the current state matches" },
    "expectActionName": { "type": "string", "description": "refuse unless topMostUndoActionName matches, e.g. 'Agent: …'" } } }
```

**Validation before mutation.**
- E_NO_SCORE: no current master notation (`nac.cpp:583-586`).
- E_NOTHING_TO_UNDO: `!canUndo()` (`nus.cpp:38-45`, `undo.h:281`).
- E_BUSY: undo stack locked or macro active (S4, S5); agent transaction in progress; drag
  (`isDragStarted`, `inotationinteraction.h:97`); text editing (S6). The UI allows undo during
  text editing (it undoes keystrokes, guarded by `startUndoIdx`, `undo.cpp:468-475`), but for the
  agent that would undo the user's typing.
- E_ACTION_NOT_ALLOWED: review page (`nac.cpp:589-591`).
- E_STALE: `expectStateVersion`/`expectActionName` mismatch (`nus.cpp:188-199`, `:223-230`).
  Needed because an agent request that changed nothing leaves *no* entry (empty macro discarded,
  notes-G1, `cmd.cpp:435-436`); a blind undo would then revert the user's previous edit.
- Note input: UI allows it; whether the agent does is Q10.

**Post-condition.** `currentStateIndex()` decreased by `steps`; `topMostRedoActionName()` equals
the name that was on top. Re-read the affected bars. No explicit `notationChanged` needed
(`nus.cpp:55`).

**Nested commands.** Undo is not a command: it never calls `startCmd`/`endCmd`; it must never run
inside a transaction (S5). Do not call it between `lock()` and `unlock()`.

**Linked parts / excerpts.** One undo stack per master score, shared by all excerpts
(`src/engraving/dom/score.cpp:6286`, `masterscore.h:100`). Each `Notation` has its own
`NotationUndoStack` wrapper and its own `notationChanged` (`src/notation/internal/notation.cpp:56`).
The UI calls undo through the *current* notation (`nac.cpp:2470-2472`); the agent should do the
same so the visible view repaints (Q11).

**Crash and corruption risks.**
- Undo inside an open macro (S5): reverted macro then deleted as redo stack at the next commit
  (`undo.cpp:426-432`). Upstream plugin precedent: a plugin calling
  `cmd("action://notation/undo")` between `startCmd`/`endCmd` hits exactly this.
- EID handles after undo can resolve to freed objects (CLAUDE.md rule 2); every handle must be
  re-validated after undo/redo.
- Plugin API caution: `cmd("undo")` is not in the compat map and dispatches the unregistered code
  `"undo"`, a silent no-op (`src/engraving/api/v1/qmlpluginapi.cpp:408-425`; UI code is
  `action://notation/undo`, `nac.cpp:52`).

**Existing upstream tests.** Engraving-level undo is exercised widely, e.g.
`src/engraving/tests/box_tests.cpp:69`, `breath_tests.cpp:70`, `keysig_tests.cpp:90-98`,
`exchangevoices_tests.cpp:103`, `beam_tests.cpp:294`, `join_tests.cpp:188`. Nothing tests
`NotationUndoStack`, `NotationInteraction::undo` or undo across a text edit.

**Scenarios.**
- SESSION-UNDO-01: agent write, then undo → export identical to before (assessment S-suite).
- SESSION-UNDO-02: batch of 10 ops, one undo → all 10 reverted (one macro, `undo.cpp:433-435`).
- SESSION-UNDO-03: no-op agent request, then undo with `expectActionName` → E_STALE, user edit kept.
- SESSION-UNDO-04: user typing in a text element → E_BUSY, text untouched.
- SESSION-UNDO-05: user mid-drag (simulated by an open macro in a unit test) → E_BUSY.
- SESSION-UNDO-06: agent write started in note-input mode, user leaves note input, agent undo →
  note-input state matches the UI's own behaviour (documented, not "fixed").
- SESSION-UNDO-07: undo with a part view current → part view repaints.

**Open questions.** Q1, Q10, Q11.

---

### redo  ·  command://notation/redo

**UI entry point.** Mirror of undo: `action://notation/redo` (`nac.cpp:53`, `:242`),
`doGlobalRedo` (`applicationactioncontroller.cpp:445-452`), `NotationInteraction::redo()`
(`ni.cpp:4648-4651`) → `NotationUndoStack::redo` (`nus.cpp:69-80`) → `Score::undoRedo(false, ed)`
(`cmd.cpp:372-398`) → `UndoStack::redo` (`undo.cpp:487-493`) → `UndoMacro::redo` (`undo.cpp:566-580`).

**Selection set-up.** None; redo restores the selection and input state recorded at the
corresponding undo (`undo.cpp:552-553`, `:575-579`).

**Arguments.** As undo (`steps`, `expectStateVersion`, `expectActionName` checked against
`topMostRedoActionName`, `nus.cpp:201-212`).

**Validation before mutation.** Same as undo, with E_NOTHING_TO_REDO for `!canRedo()`
(`undo.h:282`). Any commit (user or agent) after an undo clears the redo stack
(`undo.cpp:426-432`), so "redo after an agent write" is always E_NOTHING_TO_REDO.

**Post-condition.** `currentStateIndex()` increased by `steps`; `topMostUndoActionName()` equals
the expected name.

**Nested commands.** As undo.

**Linked parts / excerpts.** As undo.

**Crash and corruption risks.** As undo. Text editing ends with `cleanRedoStack()` when nothing
was typed (`textedit.cpp:166-170`), so the redo stack can vanish when the user merely clicks into
and out of a text element.

**Existing upstream tests.** Engraving-level redo: `git grep "undoRedo(false"` under
`src/engraving/tests`. None at notation level.

**Scenarios.**
- SESSION-REDO-01: write, undo, redo → export equals post-write export.
- SESSION-REDO-02: write, undo, user edit, redo → E_NOTHING_TO_REDO.
- SESSION-REDO-03: write, undo, redo with a stale `expectActionName` → E_STALE.

**Open questions.** Q1.

---

### save  ·  command://project/save

**UI entry point.** File > Save → action `file-save` → `saveProject(SaveMode::Save)`
(`pac.cpp:82`, `:816-849`). For an existing, not newly created, local project it goes straight
to `saveProjectAt(SaveLocation(Local))` (`pac.cpp:829-840`) → ends text editing
(`pac.cpp:963-966`) → `canSaveProject()` check with dialog on failure (`pac.cpp:968-976`) →
`saveProjectLocally(path="", Save, createBackup=true)` (`pac.cpp:978-980`, `:989-1036`) →
`onPreProjectSave` extension point (`:997-999`) → `INotationProject::save("", Save, true)`
(`:1002`) → on failure, dialogs (`:1005-1027`) → `onPostProjectSaved` (`:1030-1032`) → recent
files (`:1034`). The public `IProjectFilesController::saveProject(path)` (`iprojectfilescontroller.h:46`,
`pac.cpp:798-814`) and `saveProjectLocally` (`iprojectfilescontroller.h:47-48`) are the same
code with the same dialogs.

`NotationProject::save(path, Save, createBackup)` (`np.cpp:491-561`):
- empty path → `m_path` (`np.cpp:505-511`); suffix from that path (`:513`);
- `.mscbackup` copy if both caller and setting want it (`:515-518`; setting default true,
  `pcfg.cpp:141`; backup path `<dir>/.mscbackup/.<name>~`, `pcfg.cpp:761-766`; MSCZ only,
  `np.cpp:804-838`);
- `doSave` (`np.cpp:656-802`): writability check → `io::Err::FSWriteError` (`:667-673`); write to
  `<path>_saving` via a buffer (`:684-733`); backup (`:735-740`); copy over the target
  (`:770-773`); re-open and check the saved zip (`:775-787`, `:938-973`) →
  `CorruptionUponSavingError`; remove temp file (`:790`); chmod (`:794-798`);
- on success `markAsSaved(path)` → `needSave=false`, `m_isNewlyCreated=false`, `setPath`,
  `stackChanged` (`np.cpp:519-522`, `:1081-1093`); timestamp headers/footers relayout and
  `notationChanged` (`:524-534`); `saveComplited` (`:556-558`).
- Non-MuseScore suffix → `exportProject` writes that format over the path (`np.cpp:647-649`,
  `:975-992`).

**Selection set-up.** None.

**Arguments.** `{}`. No path: save-as and save-a-copy stay out of v1 (backup_snapshot covers
copies). If a path argument is ever added it must be confined to agent folders (CLAUDE.md rule 4).

**Validation before mutation.**
- E_NO_SCORE: no current project (`pac.cpp:789-793`).
- E_BUSY: agent request in progress; undo stack locked or macro active (S7); text editing (the UI
  would end the user's edit, `pac.cpp:963-966`; the agent must not); drag (S4).
- E_MODAL_OPEN (S11).
- E_NEEDS_SAVE_AS: `isNewlyCreated()` (`inotationproject.h:58`). True for new scores, imports and
  pre-4.0 files (`np.cpp:134-140`, `:343-344`). The UI asks for a location here
  (`pac.cpp:829-848`). Calling `INotationProject::save()` directly would instead write to
  `m_path`: the score *title* for new scores (`np.cpp:1011`), or the original import file, where
  `exportProject` would overwrite e.g. the `.musicxml` source, or the 3.x `.mscz` in 4.70
  format (`np.cpp:647-649`, `writer.cpp:59`). This check is mandatory.
- E_CLOUD_PROJECT: `isCloudProject()` (`np.cpp:461-464`); the UI uploads (`pac.cpp:834-835`,
  `:1038-1130`).
- E_NO_PARTS / E_CORRUPTED: `canSave()` (`np.cpp:1125-1138`); the UI asks "save anyway?"
  (`pac.cpp:1513-1528`). Report and refuse.
- E_FILE_TOO_NEW: file was opened with version checking disabled and is newer than 470
  (`pac.cpp:1779-1781`); saving would rewrite it as 4.70. Needs the load-time format (S8, Q12).

Failures after the call, mapped from `Ret`:
- `io::Err::FSWriteError` → E_READ_ONLY (`np.cpp:669-673`).
- `Err::CorruptionUponSavingError` → E_SAVE_CORRUPTED; the target may already be overwritten,
  the `.mscbackup` copy holds the previous version (`np.cpp:735-740`, `:770-786`). Never retry
  automatically; the UI's retry passes `createBackup=false` to protect that copy (`pac.cpp:1011-1017`).
- other → E_SAVE_FAILED with `ret.text()`.

**Post-condition.** `Ret` ok; `needSave().val == false`; `path()` unchanged; the file's mtime
changed; `.mscbackup` copy present if the setting is on. Autosave file removed by
`ProjectAutoSaver::update` on the `needSave` change (`pas.cpp:74-76`, `:125-142`).

**Nested commands.** No `startCmd` of its own except S7 (hidden parts + multimeasure rests),
which is safe only with no active macro and the stack unlocked. Never call inside a batch.

**Linked parts / excerpts.** Saves the whole project including excerpts (`src/engraving/rw/mscsaver.cpp:81-119`).

**Crash and corruption risks.**
- Recommended call: `INotationProject::save()` with the guards above, after the agent's own
  pre-checks, rather than `IProjectFilesController::saveProject()`, which cannot be made
  dialog-free (S11). Whether to also run the extension points and update recent files, as the UI
  does (`pac.cpp:997-999`, `:1030-1034`), is Q13.
- S8 side effects (thumbnail page-mode flip and full relayout, mscVersion overwrite, EIDs).
- S10: after save, undoing back to the load state reports not-dirty although the file differs.
- Autosave: the timer fires only from the event loop (`pas.cpp:34-38`, `:144-182`); it writes
  `<path>.autosave` next to the score (`pas.cpp:115-118`, `pas.cpp:170-173`), and for new scores
  `userAppDataPath()/new_project.mscz` (`pcfg.cpp:657-660`). It cannot interleave with a
  synchronous agent request unless that request spins an event loop (S3).
- Plugin API caution: there is no save in plugin API v1; `writeScore` goes through
  `ExportProjectScenario`, which asks "replace file?" and "retry?" (`engravingpluginapihelper.cpp:34-50`,
  `src/project/internal/exportprojectscenario.cpp:346-411`).

**Existing upstream tests.** None for `NotationProject::save`, `doSave` or the controller. Engraving
save/compare helpers: `src/engraving/tests/utils/scorerw.h:46-47`, `scorecomp.h:33`.

**Scenarios.**
- SESSION-SAVE-01: edit, save → needSave false, reopen in a fresh process → export identical.
- SESSION-SAVE-02: read-only file (chmod 444) → E_READ_ONLY, no dialog, needSave still true.
- SESSION-SAVE-03: new unsaved score → E_NEEDS_SAVE_AS, nothing written anywhere.
- SESSION-SAVE-04: imported MusicXML → E_NEEDS_SAVE_AS, source file byte-identical.
- SESSION-SAVE-05: user in text edit → E_BUSY, text edit still active.
- SESSION-SAVE-06: score with hidden part and multimeasure rests on → save leaves undo index and
  part visibility unchanged (S7).
- SESSION-SAVE-07: `.mscbackup/.<name>~` equals the pre-save file.

**Open questions.** Q12, Q13.

---

### backup_snapshot  ·  command://project/backup_snapshot

**UI entry point.** No UI action writes a copy without changing path or dirty state *and*
without dialogs. Candidates found:
1. `INotationProject::writeToDevice(QIODevice*)` (`inotationproject.h:76`, `np.cpp:603-641`):
   writes the full project (score, excerpts, styles, audio settings, view settings via
   `writeProject`, `np.cpp:857-902`) into a buffer then the device, with `createThumbnail=false`
   (`np.cpp:622`). No path change, no `markAsSaved`, no backup, no corruption re-check, no
   `saveComplited`. UI precedent: cloud upload (`pac.cpp:1285-1292`). Requires a non-empty
   `m_path` (`np.cpp:607-609`), which holds for new scores too (title, `np.cpp:1011`).
2. `INotationProject::save(path, SaveMode::SaveCopy, false)` (`np.cpp:502-536`): no `markAsSaved`
   for SaveCopy (`np.cpp:520-522`), but it creates a thumbnail (page-mode flip + relayout, S8),
   relayouts timestamp headers and notifies `notationChanged` (`np.cpp:524-534`), sends
   `saveComplited(path, SaveCopy)` (`np.cpp:556-558`), and with `createBackup=true` would also drop
   a `.mscbackup` in the agent folder. It does run the post-write corruption check
   (`np.cpp:775-787`).
3. `MscNotationWriter` (registered for `mscz`/`mscx`, `src/notation/notationmodule.cpp:63-64`;
   `src/notation/internal/mscnotationwriter.cpp:55-98`): omits audio and view settings, ignores
   `writeMscz`'s return value (`mscnotationwriter.cpp:88`) and creates a thumbnail.
4. The existing `.mscbackup` mechanism (`np.cpp:804-838`, `pcfg.cpp:761-766`) is a single
   hidden copy per score next to the score, overwritten at every save; it is not a snapshot
   history and writes beside the user's file.
5. Autosave (`pas.cpp`) writes `<path>.autosave` beside the score and is deleted when the
   project becomes clean (`pas.cpp:125-142`); not suitable.

Closest UI precedent with no visible side effects: option 1. Choice is the conductor's.

**Selection set-up.** None.

**Arguments.** `{ "label": { "type": "string", "maxLength": 60 } }` → output
`{ path, bytes, stateVersion, needSave }`. The file name is generated by the agent
(timestamp + score name + label); callers never supply a path.

**Validation before mutation.**
- E_NO_SCORE.
- E_BUSY: undo stack locked or macro active (S7, mandatory); agent batch in progress.
- Text editing: the write captures the text as currently typed (keystrokes are already applied,
  `ni.cpp:4454`); allowed, but the snapshot then contains uncommitted text. Q14.
- E_DISK: backup folder not creatable or not writable; target must be under
  `userAppDataPath()` (S12).

**Post-condition.** File exists, non-empty, opens as a zip with a readable score file (replicate
`checkSavedFileForCorruption`, `np.cpp:938-973`, which is private); `needSave()`, `path()`,
`currentStateIndex()` unchanged; `.autosave` and `.mscbackup` beside the user's score untouched.

**Nested commands.** Only S7. Must run before the agent opens a transaction (the assessment's
"backup before multi-bar batch" fits: snapshot, then `prepareChanges`).

**Linked parts / excerpts.** Whole project, all excerpts (`mscsaver.cpp:81-119`).

**Crash and corruption risks.**
- S7 inside an open macro rolls back the macro.
- S8: after the first snapshot of an older 4.x file, `mscVersion()` reads 470; EIDs are assigned to
  items without one. Harmless to the file, visible to hello/score_read if they report format or
  rely on "EID absent".
- Atomicity: `writeToDevice` writes the whole buffer in one `device->write` (`np.cpp:631-637`)
  but gives no temp-file-and-rename; the agent should write to a temp name and rename.
- Retention: nothing upstream prunes; the agent needs its own limit.

**Existing upstream tests.** None for `writeToDevice` or `MscNotationWriter`.

**Scenarios.**
- SESSION-BAK-01: dirty score → snapshot → needSave still true, undo index unchanged, path unchanged.
- SESSION-BAK-02: snapshot opens in a fresh process and exports identical to the live score.
- SESSION-BAK-03: snapshot during an open batch → E_BUSY.
- SESSION-BAK-04: hidden part + multimeasure rests → undo index and part visibility unchanged.
- SESSION-BAK-05: new unsaved score → snapshot succeeds, score still "newly created".
- SESSION-BAK-06: no files appear next to the user's score.

**Open questions.** Q14, Q15.

---

## Corrections to COMMANDS.md and assessment.md

1. **assessment.md line 177 (discovery path):** records in
   `~/Library/Application Support/MuseScore/MuseScore4/agent/instances/` would write into the
   *official* app's settings folder (DECISIONS 2026-10-29). The agent build's folder is
   `userAppDataPath()` under the `MuseScoreAgent` application name (S12).
2. **assessment.md line 177 vs PLAN P1.1 (port):** the assessment says "first free port from
   2212"; PLAN P1.1 says port base 2412. 2212 is the upstream rcontrol default (assessment line 5)
   and would collide with a future official MuseScore 5 on the same machine.
3. **assessment.md line 177 (routing):** the base-port holder can quit at any time (S1); the
   design needs base-port takeover by a sibling, which the assessment does not mention.
4. **assessment.md line 27:** the IPC channel also carries "is project P open?" requests, window
   activation by path and resource lock/change notices (`mpp.cpp:39-56`), not only activation,
   settings and quit. The conclusion ("nothing about score content") stands.
5. **assessment.md line 130 (stateVersion = undo index):** not unique across undo + new edit (S9).
6. **assessment.md line 130 ("refused while a save is in progress"):** the controller's
   `m_isProjectSaving` is private (`projectactionscontroller.h:228`); a save in progress is only
   observable to the agent if it happens inside a nested event loop (cloud upload,
   `pac.cpp:1299-1300`). Local saves are synchronous and cannot interleave.
7. **COMMANDS.md:** URIs are given as `command://notation/<name>` for every command; session
   commands are app/project operations (see URI note at the top).

## Open questions

- **Q1. stateVersion.** `currentStateIndex()` repeats after undo + new edit (S9). Options seen in
  source: pair the index with the top macro's identity (`topMostUndoActionName`, or the
  `UndoMacro*` from `UndoStack::last()`, `undo.h:290`), or count `ScoreChanges` notifications
  (`nus.cpp:250-257`). Pointer reuse after delete makes `UndoMacro*` unsafe as an id. Conductor to decide.
- **Q2. Base-port takeover** when the routing process quits; retry interval and race handling.
- **Q3. Port base** 2212 (assessment) or 2412 (PLAN).
- **Q4. Discovery record location and format** under `userAppDataPath()`; the exact on-disk
  folder name Qt produces for `AppLocalDataLocation` with org "MuseScore" and app
  "MuseScoreAgent" should be confirmed on the built app (G0).
- **Q5. Imports in score_open.** Allow `.musicxml`/`.mxl` (with the MusicXML error dialog risk,
  S11), or refuse in v1.
- **Q6. Corrupted files.** A corrupted file shows a dialog during load in whatever process loads it
  (`pac.cpp:1782-1786`). Loading into a throwaway `INotationProject` first
  (`IProjectCreator::newProject` + `load`, as `pac.cpp:304-314`) would detect it, but the
  migration dialog lives inside `load` (`np.cpp:188-196`). Not resolved from source whether a
  pre-load is acceptable cost for large scores.
- **Q7. Path canonicalisation.** `IFileSystem::absoluteFilePath` (`pac.cpp:248`) and literal
  `path_t` equality (`pac.cpp:662`); not checked whether symlinks or case differences on APFS
  are normalised.
- **Q8. Focus.** Case 5 of score_open starts a new window that takes focus; `activate: false`
  cannot be honoured through `openNewWindow` (no such argument, `mpp.cpp:258-304`).
- **Q9. Child processes.** Children get only the file path (`pac.cpp:268-275`); whether they inherit
  the parent's environment (for an agent token or port hint) depends on `QProcess::startDetached`
  behaviour, not visible in this tree.
- **Q10. Undo in note-input mode.** The UI allows it (`nac.cpp:593-595`); allow for the agent too, or E_BUSY.
- **Q11. Which notation's undo wrapper.** The UI uses the current notation (`nac.cpp:2470`).
  Not verified whether undo through the master's `NotationUndoStack` repaints an open part tab,
  since each wrapper notifies only its own `notationChanged` (`notation.cpp:56`, `nus.cpp:279-282`).
- **Q12. Load-time file format.** Needed for E_FILE_TOO_NEW on save and for hello; must be captured
  before the first write (S8). No upstream field keeps it.
- **Q13. Extension points and recent files on agent save.** The UI runs `onPreProjectSave` /
  `onPostProjectSaved` (plugins, synchronous, may show UI) and prepends recent files.
- **Q14. Snapshot during text editing:** allow (captures in-progress text) or E_BUSY.
- **Q15. `IInteractive::isCurrentUriDialog()`** (`iinteractive.h:252`) as the E_MODAL_OPEN test:
  not verified that it covers `warningSync`/`errorSync` message boxes and the migration dialog.
