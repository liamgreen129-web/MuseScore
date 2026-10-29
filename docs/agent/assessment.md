Claude in MuseScore: Engineering Assessment and Build Plan
28 Oct 2026 · @Liam
Verdict
Yes, this is achievable, but only by making it native: a C++ module compiled into MuseScore itself, not a QML plugin relaying to an outside process. The plugin route can be improved, but it cannot reach "as reliable as a built-in feature": it is a JavaScript layer with gaps, its lifetime is an accident of the plugin runner rather than a contract, and a fault inside it takes the whole app down or leaves its undo stack locked. MuseScore's own 4.7.6 release notes (26 Oct 2026) fix a crash caused by a plugin calling cmd("add-hairpin") between startCmd() and endCmd(), which is exactly the class of failure you are seeing.
The decisive fact, verified in source this week: MuseScore's core developers are building this capability themselves for Studio 5. The framework now contains an in-process MCP server (muse::rcontrol, streamable HTTP on 127.0.0.1:2212/mcp) sitting on a new typed command registry (muse::rcommand) with an Availability::MCP flag per command. It was merged on 26 Oct 2026 and so far exposes one read-only notation tool (get-noteinput-state); 5.0 has no public release date. It is nowhere near usable, but the direction is set, and it confirms that the native, in-process design is the one the architects chose.
Recommendation. Fork MuseScore Studio 4.7.6 (the stable line you run) and add a native "agent" module that does two things: host the same MCP-over-HTTP server design that 5.0 uses, and implement a complete, transactional score read/write command set directly on the notation and engraving layers. Shape the commands to the 5.0 conventions (command://notation/..., typed input/output schemas) so the work rebases onto 5.0 and can be proposed upstream. Claude then connects straight to MuseScore (Claude Code over HTTP; the Claude desktop app through a stateless stdio bridge), with no middle process that can lose state.
The real obstacles, honestly stated:
1. You will run your own MuseScore build. That means a C++/Qt/CMake toolchain, a first build of roughly an hour on an Apple-silicon Mac, incremental rebuilds in minutes, and a rebase for each MuseScore release (point releases arrived about monthly in 2026).
2. The API design is the hard engineering, not the plumbing: deciding what the agent sees (the score representation) and specifying roughly 40 write operations, each atomic, validated and undoable, built on the same code paths the UI uses.
3. Reliability has to be engineered in: a scenario test suite and a randomised soak test are part of the deliverable, not an afterthought.
4. MuseScore 5 will move the ground (framework split into a submodule, a new command system, and a 5.00 file format already on master). The plan includes the port rather than pretending it will not happen.
5. A better bridge does not fix musical misjudgement. The design therefore includes a verify-then-report loop so you always see what was actually changed.
A solid first version is roughly 8 to 11 engineering weeks (phased in the build plan), with Claude Code doing most of the coding and you building and testing on your machine.
What "built-in" actually requires
"As reliable as a native feature" is a measurable bar, not a feeling. Every design choice below is judged against these seven requirements, and the build plan's acceptance tests are derived from them.
1. Complete reading. The agent sees the open score exactly as MuseScore holds it in memory, including unsaved edits: every note, rest, voice, tie, tuplet, articulation, dynamic, slur, hairpin, line, text, chord symbol, lyric, key, time and tempo change, part, staff, instrument, layout break and style setting. It gets this at two grains: a whole-piece overview small enough to reason about, and bar-by-bar detail on demand.
2. Complete writing. Any edit the UI can make is expressible as an explicit operation. Each request applies atomically, lands as one undo step with a readable name, triggers a normal relayout, and leaves the selection somewhere sensible. No edit happens through simulated keystrokes or mouse moves.
3. Reliability. No crash attributable to the integration, ever; the target in testing is zero crashes across a randomised soak run of at least 24 hours. The link survives MuseScore restarts, score switches, score closes and sleep/wake without a manual "reconnect" step. Small edits complete in under 100 ms end to end on the local machine; a full read of a 30-page score completes in under 2 s; long operations report progress and can be cancelled; every failure returns a specific error rather than a timeout.
4. Native feel. It works with whatever score is open, without launching anything by hand. Changes appear in the score view immediately. In-app undo and redo work on them. Dirty state, autosave, parts and linked staves behave normally.
5. Safety. The agent cannot destroy work: every change is undoable, a backup snapshot precedes any multi-bar edit, and the channel is local-only and authenticated.
6. Maintainability. A new MuseScore release costs a bounded amount of work (a rebuild or a rebase), and an automated test suite says whether anything broke.
7. Self-verification. The agent can see the result of what it did (rendered pages, exported audio or MIDI, a structural diff) before telling you it is done.
Requirements 2, 3 and 4 together are what rule out most of the approaches considered in the comparison section: a bridge that simulates the UI, or that depends on a scripting layer with gaps, cannot meet them no matter how carefully it is wrapped.
How MuseScore is built: the facts that constrain the design
Everything below was checked against the MuseScore source this week: the v4.7.6 tag (your line) and master at commit 38f00e3 of 27 Oct 2026 (the 5.0 line), plus the muse_framework repository at commit ecdc6e7. Paths are relative to the repository root.
Editing happens on one thread, inside command brackets. The score model (src/engraving/dom, classes Score, Measure, Segment, Chord, Note and so on) is edited only on the GUI thread, between prepareChanges(name) and commitChanges() on INotationUndoStack (src/notation/internal/inotationundostack.h). A commit lays the score out again, notifies the view, and leaves exactly one named entry in the undo history; rollbackChanges() discards everything since the prepare. This is the mechanism behind every UI action, and it is the mechanism an agent must use to get "one request, one undo step" for free.
The notation layer is the real API. INotation (src/notation/inotation.h) hands out interaction() (210 virtual methods in inotationinteraction.h: note input, selection, adding and removing elements, transposition, measures, voices, text, lines and more), undoStack(), parts(), style(), painting(), elements() (which also exposes the raw mu::engraving::Score* through msScore()) and change notifications (notationChanged()). Everything the UI does goes through these, so a module that calls them inherits the UI's tested behaviour. The plugin API is a thin JavaScript veneer over a subset of the same layer.
Change notifications are already structured. After each command, INotationUndoStack::changesChannel() publishes a ScoreChanges record (src/engraving/dom/score.h): the affected tick and staff range, the changed objects with their command types, the changed element types, properties and style ids. That is the raw material for a "what changed since version N" feed and for keeping an agent's view of the score current without re-reading it.
Elements have stable identifiers, with one trap. Every engraving object can carry an EID (two 64-bit values, serialised as base64; src/engraving/infrastructure/eid.h), assigned on demand and written into saved files since the 4.10 file format. MasterScore::eidRegister()->itemFromEID() resolves one back to a pointer. The trap: the register only unregisters objects in specific read-time cases (eidregister.cpp, removeItem), so an EID can resolve to a freed object after an undo or a delete. The design therefore treats EIDs as handles that must be re-validated against the live score tree before use, never as pointers to trust.
The plugin API is a JavaScript wrapper with real gaps and no error channel. Plugin API v1 lives in src/engraving/api/v1. Its wrappers hold raw pointers with no invalidation (scoreelement.h), a new wrapper is created on every access, and nothing tells a plugin when an element was freed, so a stale wrapper reads freed memory. Cursor.add has no code path for spanners at all (slurs, hairpins, ottavas, pedal lines hit an assert in segment.cpp), ties and grace notes have no element-level API, and measures cannot be inserted at a position except one at a time through cmd("insert-measure") with a selection; the count-taking variants open a modal dialog because cmd() cannot pass arguments. Errors never reach JavaScript: bad input returns null or logs at debug level. Worse, a JavaScript exception between startCmd() and endCmd() leaves the undo stack locked, after which MuseScore's own commands silently do nothing (cmd.cpp, startCmd/endCmd early returns). cmd() has no allow-list, so a plugin can dispatch quit or file-save. Plugins run through the extensions framework (src/framework/extensions), which triggers an action manually or at four project events (opened, created, before save, after save), never once at application start; in 4.7.6 the user interface for choosing those points is commented out (EnablePanel.qml), so they can only be set by editing config.json by hand. Official builds are compiled with MUSESCORE_BUILD_WEBSOCKET=ON (buildscripts/ci/macos/build.sh), which gives plugins the api.websocket and api.websocketserver objects mcp-score relies on; that server binds to all network interfaces with no authentication and no way to stop it (websocketserverapi.cpp).
Exports run on the in-memory score. INotationWriter::write(notation, device, options) (src/project/inotationwriter.h) is implemented for mscz/mscx and, through the import-export modules, for MusicXML, MXL, MNX, MEI, MIDI, PDF, SVG, PNG, WAV, MP3, OGG, FLAC, AAC and MP4. A native module can therefore hand the agent a lossless MusicXML or mscx snapshot, or a rendered PNG, of exactly what is on screen including unsaved edits.
Modules are pluggable at build time, not at run time. The application is assembled from IModuleSetup modules listed in src/app/appfactory.cpp; adding one is a new directory, a CMake target and a registration line. There is no mechanism to load a native module into an official binary, so native code means a fork.
Each score window is a separate process. In 4.7.6 the multi-window module uses MultiProcessProvider unless the work-in-progress MUSE_MULTICONTEXT_WIP option is on; opening a second score calls QProcess::startDetached on the application itself (src/framework/multiwindows/internal/multiprocess/multiprocessprovider.cpp). The windows coordinate over a QLocalServer channel that only carries window activation, settings transactions and quit requests, nothing about score content. Any in-process server therefore exists once per open score, and the design has to say which process Claude is talking to.
MuseScore 5 is where the upstream agent work is happening. On master: version 5.0.0 (prerelease), the framework moved into a muse git submodule, scores are written as format 5.00, the action system is being replaced by muse::rcommand (commands as URIs such as command://notation/get-noteinput-state, typed input and output schemas, Availability flags including MCP), and muse::rcontrol adds an in-process MCP server: streamable HTTP on 127.0.0.1:2212/mcp with origin checks, protocol version 2025-11-25, a TCP transport, and a 150-line musescore-mcpbridge executable that bridges stdio to it. One notation command carries the MCP flag today. The server starts whenever the GUI app starts (muse_framework PR #377, merged 26 Oct 2026).
Build facts. CMake and Ninja, Qt 6.8 or newer (4.7 ships Qt 6.10.2), C++17, static libraries, macOS deployment target 10.15. buildscripts/ci/macos/build.sh and ninja_build.sh show the exact option set the official macOS build uses, which is the set a fork should copy. Point releases in 2026 landed on 13 May, 19 May, 26 May, 11 Jun, 7 Jul, 8 Sep and 26 Oct (GitHub releases).
Why the current bridge is fragile
The three symptoms you named (the app closes, the link loses state, calls time out) map onto structural properties of a QML-plugin bridge, not onto bugs in one version of mcp-score. Fixing them one by one would leave the structure in place.
The crashes belong to the plugin API, not the bridge. The QML plugin layer wraps raw pointers to score elements and runs JavaScript on the GUI thread, and several wrong calls end the process instead of returning an error. mcp-score's own integration tests found that addChordSymbol segfaulted (a chord symbol parsed its text before it had a parent) and that adding a bar line through Cursor.add crashed MuseScore; MuseScore 4.7.6 separately fixed a crash from cmd("add-hairpin") inside a startCmd/endCmd pair (PR #98, 4.7.6 notes). In a native module these become error returns or are impossible by construction; in a plugin, one bad call costs you the app and anything since the last autosave.
State lives in four places, and only one of them should exist. mcp-score's bridge is a dialog plugin, so its window must stay open (its connect tool states this): closing it, closing the score or relaunching MuseScore kills the WebSocket server, and your own connect procedure then resorts to driving the Plugins menu by computer use. A windowless plugin can stay resident, but only because the plugin runner never deletes the object it creates; nothing restarts it, nothing can stop its server, and the plugin is never told that the score changed or closed (scoreStateChanged has no emitter in 4.7.6). The Python MCP server is a second process, launched by the Claude desktop app, and your own connect notes record the desktop app running a stale build of it with the wrong port. The WebSocket connection and the plugin's JavaScript cursor position are the third and fourth. Any of the four restarting loses the session, and one more failure is invisible: a JavaScript error between startCmd() and endCmd() leaves MuseScore's undo stack locked, so the app itself stops recording edits until it is restarted. The only state that should exist is the score.
Timeouts come from the chain's length and from blocking on the GUI thread. A call from a claude.ai session travels cloud → desktop app → Python server → WebSocket → QML JavaScript → engraving code, and back, and every hop has its own timeout. The JavaScript side shares the GUI thread, so a long plugin operation, a relayout or a modal dialog stalls the socket; the call times out even though the edit may still complete, which is how "it timed out" and "it did it twice" both happen.
The API is incomplete and moves between releases. Score.transpose is not exposed, so mcp-score reimplements transposition in JavaScript; the undo action code changed in 4.7 (action://notation/undo), which silently turned undo into a no-op until the plugin special-cased versions; and much of the editing surface is reachable only through cmd("..."), which depends on the current selection and is where the hairpin crash lived. Coverage is bounded by what the wrappers expose, not by what MuseScore can do.
Nothing keeps it resident. The 4.4+ extensions framework can run a plugin automatically, but only at four points (project opened, project created, before save, after save), never once at application start. Setting the bridge to run when a project opens re-runs it, window and all, for every score you open, and the second copy cannot bind the port the first one holds. mcp-score's CI therefore presses the plugin shortcut with xdotool on Linux and osascript on macOS to bring the bridge up; that is the clearest sign that the plugin model has no place for a service that is simply there.
Approaches considered
Eight ways to put an agent inside MuseScore were weighed against the requirements above. Only one meets requirements 2, 3 and 4 at the same time; the others fail on capability, on the crash surface, or on what has to stay running.
Approach
Read
Write
Reliability ceiling
Feels built in
Build effort
Per-release upkeep
Upstream path
A. Harden the QML plugin bridge (current model, done well)
Slow JS traversal; lossless only via export
Bounded by plugin API v1 plus cmd()
Medium: one wrong call still kills the app
No: a plugin window must stay open; auto-run exists only per project open
2 to 4 weeks
Action codes and API quirks change per release
None
B. Rebuild on the 4.4+ extensions framework (manifest, api.* objects, WebSocket API)
Same as A
Same as A (editing still goes through v1)
Medium
Partly: cleaner lifecycle, same crash surface
3 to 5 weeks
Same as A
None
C. Autobot test-case script launched at startup
Same as A
Same as A plus action dispatch
Low: an internal test harness, undocumented
No: special launch flags
2 to 3 weeks
High
None
D. File round trip (edit .mscx or MusicXML outside, reload)
Lossless
Lossless but offline: no undo, no selection, reload required
High for reads, poor for live editing
No
2 to 3 weeks
Low
None
E. UI automation (computer use, accessibility, keystrokes)
Screenshots only
Anything a human can click, slowly
Low
No
Ongoing
Medium
None
F. Native C++ module in a 4.7.6 fork (recommended)
Lossless and fast, native serialisation
Everything the UI can do, transactional, undoable
High: same code paths as the UI, no scripting layer
Yes: in-process server, starts with the app, one undo entry per request
8 to 11 weeks
A rebase and rebuild per release, tests to prove it
Yes, by matching 5.0's command conventions
G. Build on MuseScore 5 development (rcommand + rcontrol) now
As F
As F
High eventually; prerelease today
Yes
As F, minus the server
Moving target until 5.0 ships
Best, same code
H. Wait for MuseScore 5.x to ship its own MCP
n/a
n/a
High, when it exists
Yes
None
None
n/a
Why F wins. It is the only option whose capability is bounded by MuseScore itself rather than by a wrapper layer, and whose failure modes are MuseScore's own, which the project tests and fixes. It removes the two things that currently break: the external process and the plugin window. Its cost is real but bounded and mechanical (a toolchain and a monthly rebase), whereas the cost of A and B is open-ended: every new capability is another workaround and every release a new set of silent breakages.
Why not G today. MuseScore master already writes score files as format version 5.00 (Constants::MSC_VERSION = 500 on master against 470 on 4.7.6), has moved the framework into a muse submodule, and is replacing the action system with the new typed command registry. Running it as a composer's daily tool means living on a prerelease whose saved files 4.7 treats as foreign. G is the right end state and the right place to contribute, which is why F mirrors its conventions; it is not the right place to do your composing this winter.
Why not H. As of 27 Oct 2026 the upstream MCP server exposes one tool, has no release date, and the team's stated 5.0 priorities are playback automation and shared staves, not a score-editing API. Waiting means an unknown number of months with no influence over scope; the recommended plan reaches the same destination with working software along the way.
Kept from the rejected options. Headless rendering through the MuseScore command line (from D) remains a useful verification tool, and computer use (from E) remains the fallback for dialogs that need a human. Neither carries editing traffic.
Recommended architecture
The whole design reduces to one rule: the score is the only state, and the only code that touches it is native code running on MuseScore's own command path. Three new components live inside MuseScore; nothing new runs outside it except Claude.
Claude calls the MCP server over HTTP; the registry turns the call into a typed command; the score service executes it as one transaction on the existing notation interfaces; the engraving layer lays out, records one undo entry and repaints. Renders and exports flow back to Claude for verification.
Components
• Agent module (src/agent, a new IModuleSetup registered in appfactory.cpp). Owns the other three and a preferences page (enable, port, token, logging). It is compiled only when MUE_BUILD_AGENT_MODULE=ON, so the same fork can produce a vanilla build.
• MCP server. A port of 5.0's muse::rcontrol server (mcpserver.cpp, httptransport.cpp, about a thousand lines that depend only on Qt Network and the framework's JSON and Val types, all present in 4.7.6). Streamable HTTP on 127.0.0.1:2212/mcp, loopback only, with the upstream origin check plus a bearer token kept in MuseScore's application-support folder. Listening starts with the app and stops with it; there is no window and nothing to click.
• Command registry. The same shape as 5.0's rcommand::CommandInfo: command URI, title, description, typed input and output schemas, availability flags. The MCP tool list is generated from it, so Claude always sees accurate JSON schemas, and the registry doubles as the API's documentation and its test manifest.
• Score service. The only code that reads or writes the score. It resolves addresses, validates every argument against the live model, runs each request inside prepareChanges/commitChanges, and builds the response from the post-commit state. It calls INotationInteraction, INotationParts, INotationStyle and, where the interaction layer has no entry point, mu::engraving::Score directly through elements()->msScore(), always the same functions the UI uses.
Execution model
Qt sockets belong to the thread that creates them, so the server's handlers already run on the GUI thread, which is the only thread allowed to edit the score; no marshalling is needed. Each request is one transaction named for the undo history (Agent: reharmonise bars 12 to 19); a batch request applies several operations in one transaction and is all-or-nothing, with rollbackChanges() on the first failure. Validation runs before any mutation, so a bad argument returns a structured error (E_RANGE, E_VOICE_OCCUPIED, E_NO_SCORE, E_MODAL_OPEN) and leaves the score untouched. Requests are refused, not queued, while a modal dialog is open or a save is in progress; playback is stopped before structural edits. Every response carries stateVersion (the undo index) so Claude can tell whether the score moved under it, and long batches report progress through MCP progress notifications and can be cancelled between operations.
Two properties of the interaction layer shape the service's internals. First, it is selection-driven: addSlurToSelection(), addHairpinsToSelection(type), toggleArticulationForSelection(id), addTupletToSelectedChordRests(options), transpose(options) and deleteSelection() all act on the current selection (inotationinteraction.h, lines 186 to 320). The service therefore saves the user's selection, sets the selection to the addressed range or elements, calls the method, and restores the selection, all inside the transaction. Second, most of these methods return void: they do not report failure. Every write command therefore checks its own post-condition (the hairpin exists at the range, the bar sums correctly, the note has the pitch asked for) and rolls back and returns an error when the post-condition does not hold, rather than reporting success because a call returned.
The transaction itself follows the pattern the plugin API already uses (src/engraving/api/v1/score.cpp, lines 591 to 622): prepareChanges(name), then lock() on the undo stack, the edits, unlock(), then commitChanges() or rollbackChanges(). The lock matters because Score::startCmd is not re-entrant and several engraving helpers (cmdAddTie, cmdToggleTie, cmdEnterRest) open and close their own command, which would close the outer one early; with the stack locked those inner calls become no-ops and everything lands in one macro. Two more details from cmd.cpp: endCmd forces a rollback when MScore::setError has recorded a non-warning error, which the service turns into a structured error rather than a silent no-op; and commitChanges() does not notify notationChanged, so the service notifies it itself, as NotationInteraction::apply() does. Layout after a commit is range-based, driven by the ticks the undoable helpers record, so the service never calls layout directly.
Read model: what Claude sees
Reading is the half that determines whether Claude "understands the score fully", and it is designed in three grains so that no single call has to carry a whole symphony (Claude Code warns above 10,000 tokens of tool output and truncates at 25,000).
Overview. score_overview returns parts, staves, instruments, clefs and transpositions; the measure count; every time, key and tempo change with its bar number; rehearsal marks, sections, repeats and jumps; which bars are non-empty per staff; the current selection; the dirty flag and the file path. A few hundred tokens for most pieces.
Range read. score_read(range, staves, voices) returns the content of a bar range in a compact notation that is lossless for pitch, rhythm, voice, ties, tuplets and grace notes, with dynamics, articulations, slurs, hairpins, lines, text, chord symbols and lyrics attached as annotations. The same notation is the input language of the write operations, so Claude reads and writes in one vocabulary. Two bars of one staff look like this:
# part 2 "Trombone", staff 2, bass clef, sounds as written, 4/4, Eb major
m12 v1 | Bb3:q. C4:e D4:q~ D4:q        mf   cresc->m13
m12 v2 | r:h [G3,Bb3]:h
m13 v1 | (Eb4:h F4:q G4:q)              "dolce"  {Cm7}
Pitch is spelled with its octave; : introduces the duration (w h q e s t x, dots allowed); ~ ties to the next note; ( ) bracket a slur; [ ] a chord; r a rest; {3:2 e e e} a tuplet; g(E4:s) a grace note; annotations follow the events. A detail flag returns the same range as JSON with element handles and every property, for the cases where the notation is not enough.
Export and render. score_export(format) writes a MusicXML, mscx or MNX snapshot of the in-memory score through the registered writers, into a memory buffer the way the converter module does (backendapi.cpp), and score_render(range or page) writes PNG pages through paintPng or a bar range cropped from its page by the selection's canvas rectangle, both to a temporary folder for Claude to open. The export scenario forces page view, so the service switches the view mode and restores it. This is also how Claude checks its own work.
score_changes(since: stateVersion) returns the bars and staves touched since a given version, built from the ScoreChanges records the undo stack already publishes, so a long session re-reads only what moved.
Write model: what Claude can do
Group
Operations
Built on
Notes and rhythm
write_voice (replace a voice's content over a bar range using the compact notation, the workhorse for reharmonising and rewriting), add_note_to_chord, remove_note, set_pitch, set_duration, tie, tuplet, grace, clear_range, exchange_voices
setNoteRest, addNote, cmdDeleteSelection on a range selection filtered to one voice, addTupletToSelectedChordRests, cmdExchangeVoice
Transposition
transpose(range, by interval or to key, options for key signatures and chord symbols)
the native transpose used by the Transpose dialog
Expression
add_dynamic, add_hairpin, add_slur, add_articulation, add_ornament, add_fermata, add_breath, add_text(kind: staff, system, expression, tempo, rehearsal), add_chord_symbol, add_lyric(verse), add_line(kind: pedal, ottava, trill, volta, text line)
element creation through INotationInteraction
Structure
insert_measures, remove_measures, split_measure, join_measures, set_time_signature, set_key_signature(global or per staff), set_tempo, set_barline, set_repeat, add_break(system, page, section), add_clef
measure and signature commands
Instrumentation
add_part, remove_part, rename_part, set_instrument, add_staff, set_staff_type, set_transposition
INotationParts
Generic
set_property(handle, name, value) over the full Pid property system, remove(handle), select(range or handles), cmd(action) restricted to an allow-list
undoChangeProperty, undoRemoveElement, selection, action dispatch
Style and layout
set_style(id, value), set_measure_stretch, lock_systems, set_offset
INotationStyle, layout commands
Session
undo, redo, save, backup_snapshot, scores_list, score_open, hello (versions and capabilities)
project and undo interfaces
write_voice deserves its own line because it is where most musical work happens: it parses the compact notation, checks that each bar sums to the time signature (and reports exactly where it does not), deletes the voice's existing content in the range, and enters the new content the way note input would, including ties across bar lines and tuplets. Reharmonising a passage is one batch of write_voice calls plus chord symbols, committed as one undo entry.
set_property names properties by their C++ identifiers (HAIRPIN_TYPE, PLACEMENT, OFFSET), not by their XML names, because the XML names are not unique: subtype is shared by seven different properties, so the engine's own propertyId(name) lookup cannot tell them apart (property.cpp). Value conversion follows the plugin API's ScoreElement::set (scoreelement.cpp), which already maps JSON-like values onto PropertyValue by the property's type and marks styled properties unstyled on write.
Addressing
Positions are structural and human-readable: {measure: 12, beat: 2.5} or {measure: 12, tick: 960}, a staff by part name and staff index, a voice 1 to 4. Elements returned by a read carry a handle: the element's EID plus its structural location. Before any use, the service walks the live score to the handle's structural location (measure, segment, track) and accepts the handle only if the object found there is the one the EID register maps to; it never dereferences the register's pointer first. A stale handle returns E_STALE_HANDLE instead of touching freed memory. The service holds no pointers between requests.
Several scores open at once
Because 4.7.6 opens every score window in its own process, the module runs once per open score. Each instance binds the first free port from 2212 upward and writes a discovery record (process id, port, token, score path, window title) to ~/Library/Application Support/MuseScore/MuseScore4/agent/instances/, removed on exit and pruned by liveness on start. Claude's configuration points at port 2212, and the instance holding it answers for itself and routes for its siblings: every command accepts an optional instance argument, instances_list enumerates the records, and a request naming another instance is forwarded verbatim to that process's port with its token. No routing state is kept between calls, so a sibling closing mid-session produces a clean E_INSTANCE_GONE rather than a hang. When 5.0's single-process multi-window mode lands (MUSE_MULTICONTEXT_WIP in the current tree), the same instance argument maps onto its contexts and the forwarding disappears.
Verification and safety
Every write returns the compact notation of the bars it touched, so Claude confirms the result before reporting it. score_render gives it a picture when layout matters. backup_snapshot writes a timestamped .mscz copy before any multi-bar batch (the agent's skill calls it automatically). Nothing saves the file unless asked. The module never calls exit, never shows dialogs, and never writes outside its temporary folder and the backup folder.
The Claude side
No server runs outside MuseScore. Claude Code connects with claude mcp add --transport http musescore http://127.0.0.1:2212/mcp --header "Authorization: Bearer <token>"; the desktop app uses the stdio bridge (the upstream musescore-mcpbridge, or mcp-remote). A skill, musescore-agent, carries the working method: read the overview, read the bars in play, plan, write as one batch with a named transaction, verify by re-reading and rendering, then report in musical terms. The skill also carries the notation legend and the rule that the generic cmd escape hatch is for listed actions only.
A chat panel inside MuseScore is deliberately not in this plan. The agent loop, context handling, skills and model access already exist in Claude Code and the desktop app; rebuilding them in QML would be a second product, not a better bridge. What makes the experience feel built in is that edits land in the score instantly as named undo steps while you keep working in MuseScore, and a status indicator shows what the agent is doing. If a panel is wanted later, the command surface built here is exactly what it would sit on.
A session, concretely
You have a brass quintet open and say: "Reharmonise bars 12 to 19 so the bass moves chromatically under the tune, and put chord symbols on it." Claude calls score_overview (a few hundred tokens), then score_read for bars 12 to 19 across all five staves, and plans the harmony from the compact notation. It calls backup_snapshot, then one batch containing write_voice for the tuba and trombone voices in those bars and eight add_chord_symbol calls, labelled Agent: reharmonise bars 12 to 19. MuseScore applies it in one transaction: the bars redraw, the status indicator flickers to "working" and back, and Edit > Undo now reads "Undo Agent: reharmonise bars 12 to 19". Claude re-reads the eight bars, renders them to a PNG, looks at it, and tells you what it changed, bar by bar, with the chord symbols it chose. If you dislike it, one Cmd-Z removes all of it. Elapsed time from your sentence to the redraw is a few seconds, nearly all of it Claude thinking; the MuseScore side takes tens of milliseconds.
Build plan
The work is split into six phases, each closed by a gate that is a test result, not a feeling. Reading ships before writing because a correct read model is useful on its own and because every write test is checked by reading. Nothing in a later phase starts until the previous gate passes.
Weeks are engineering weeks with Claude Code doing the coding and you building, testing and composing with the result; calendar time depends on how many hours a week you give it.
Phase 0 · Toolchain and vanilla build (week 1)
• Fork musescore/MuseScore at tag v4.7.6; branch agent/4.7. Install Xcode command-line tools, CMake, Ninja and Qt 6.10.2, taking Qt from the same prebuilt archive the official CI uses (`buildscripts/ci/macos/install_qt.sh` downloads it from the cbjeukendrup/musescore_build_qt releases), which avoids the Qt installer and matches the shipped binaries.
• Build with exactly the options buildscripts/ci/macos/build.sh uses, in RelWithDebInfo: fetch the prebuilt dependencies with buildscripts/ci/macos/download_dependencies.sh, then run ninja_build.sh -t installrelwithdebinfo with MUSESCORE_MACOS_DEPS_PATH pointing at them and MUSESCORE_BUILD_WEBSOCKET=ON for parity with the official binary. Building in dev mode (MUSE_APP_BUILD_MODE=dev) gives the app its own settings name, MuseScore4Development, which is what makes side-by-side use clean. Note that MUSE_MODULE_MUSESAMPLER_LOAD_IN_DEBUG is off by default, so a Debug build will not load Muse Sounds; the daily-driver build is not a Debug build.
• Install the result beside the official app and use it for a week. One thing the source cannot tell us is whether the proprietary Muse Sounds library accepts a self-built, unsigned host binary; gate G0 exists to answer that on your machine before anything is built on top. Muse Sounds load from Muse Hub's library at ~/Library/Application Support/MuseScore/MuseSampler/lib/libMuseSamplerCoreLib.dylib, which any build finds.
• Write scripts/sync-upstream.sh (fetch tag, rebase, rebuild, run tests) now, while the branch is still empty, so the monthly ritual exists from day one.
Phase 1 · Module skeleton and reading (weeks 2 to 3)
• Add src/agent with the module, the ported MCP server and transport, the registry, a preferences page and a token file. Claude Code connects and sees hello.
• Implement score_overview, score_read (compact notation and JSON detail), score_export, score_render and score_changes.
• Build the fixture corpus: your ten to twenty real scores plus synthetic stress scores (every tuplet ratio, nested tuplets, four voices, cross-staff beams, linked parts, chord symbols, lyrics with melismas, grace notes, ties across bar lines, pickup bars, irregular bars).
• Gate G1: for every fixture, score_read of the whole piece re-entered through the notation parser reproduces the original pitch and rhythm content exactly; score_export matches a save from the UI; a 30-page score reads in under 2 s.
Phase 2 · Transactions and core writes (weeks 4 to 6)
• Transactions, batch, rollback, undo naming, stateVersion, structural errors.
• write_voice first (it exercises everything: deletion, note entry, ties, tuplets, chords, rests, grace notes), then set_pitch, transpose, add_dynamic, add_hairpin, add_slur, add_articulation, add_text, add_chord_symbol, add_lyric, add_line.
• The scenario suite S1 to S20, each a script against a fixture with an expected export: single dynamic; dynamic on an existing chord; slur across a system break; hairpin spanning three bars; rewrite a 4-bar melody; add a second voice under an existing one; reharmonise 8 bars with chord symbols; transpose a passage preserving spelling; transpose to a new key with key signature change; add lyrics with melismas; tuplet entry; tied notes across the bar line; grace-note entry; undo after each of these restores the previous export byte for byte; redo restores the edit; a batch with a deliberate failure in step 7 leaves the score unchanged; concurrent user edit between read and write is detected by stateVersion; an edit while a modal is open is refused; an edit during playback stops playback first; an edit to a linked part shows in the score and the part.
• Gate G2: S1 to S20 pass; an 8-hour run of random valid operations on random fixtures ends with zero crashes and all invariants holding (every bar sums to its time signature, no dangling spanner ends, undo to the start restores the original export).
Phase 3 · Structure, instrumentation and the generic layer (weeks 7 to 8)
• insert_measures, remove_measures, split_measure, join_measures, set_time_signature, set_key_signature, set_tempo, set_barline, set_repeat, add_break, add_clef; add_part, remove_part, set_instrument, add_staff, set_transposition; set_property, remove, select, set_style, cmd with its allow-list.
• Scenarios S21 to S40 cover these, plus the ugly cases: deleting bars that contain spanner endpoints, time-signature changes inside tuplets, key changes with transposing instruments, removing a part that other parts' chord symbols depend on, and property changes on linked elements.
• Gate G3: S21 to S40 pass; a 24-hour soak of random operations under AddressSanitizer and UndefinedBehaviorSanitizer on a separate Debug build stays clean; every scenario's undo and redo round-trips.
Phase 4 · Polish and daily use (weeks 9 to 10)
• A status indicator in MuseScore's status bar (connected, idle, working, with the current transaction name) and an "Agent" section in Preferences (enable, port, regenerate token, open log).
• Progress notifications and cancellation for long batches; latency measurement built into the server log.
• The musescore-agent skill: working method, notation legend, when to render, when to back up, how to report. Tool descriptions tuned on transcripts of real sessions.
• Gate G4: two weeks of your real composing with the agent, no manual reconnects, no restarts, no crashes; small edits under 100 ms and full reads under 2 s as measured by the log.
Phase 5 · MuseScore 5 port and upstream (ongoing)
• Open a GitHub discussion with the authors of rcontrol and rcommand describing the command set and the compact notation, and ask which parts they would take.
• Rebase the score service onto master, replacing the ported server with the real rcontrol and registering the commands through rcommand with Availability::MCP. Submit the read commands first (small, low-risk, easy to review), then the transactional write layer.
• Until 5.0 ships, keep the 4.7 branch as the daily build and rebase it on each 4.7.x point release.
Test rig
Four layers, all runnable from one ctest invocation and on a GitHub Actions macOS runner:
1. Unit tests in src/agent/tests using MuseScore's existing engraving test harness (gtest, scores loaded from .mscz fixtures): notation parser and printer, address resolution, every command's validation paths.
2. Scenario tests: a Python driver launches the built app with QT_QPA_PLATFORM=offscreen and --agent-port, runs each S-script over MCP, exports, and compares with golden files under tests/golden.
3. Fuzz and soak: the same driver generating random valid operation sequences from the registry's schemas, with invariant checks after every transaction and a crash-dump collector.
4. Performance budgets: timings recorded per command, with thresholds that fail the run.
The golden files are the honest arbiter: when a MuseScore release changes layout or export output, the diff shows exactly what moved, and the suite is re-baselined deliberately rather than silently.
Biggest risks and what could go wrong
The first three are the ones that could sink the project; the rest are costs to budget for. Each has a specific mitigation that is already part of the plan above, not a hope.
Risk
What it would look like
Likelihood · impact
Mitigation in the plan
MuseScore's own editing code crashes when driven in sequences the UI never produces (empty selection, spanner ends in deleted bars, edits to linked staves)
The same symptom you have today: the app vanishes mid-edit
Medium · High
Only high-level interaction methods with explicit selection set-up; validation before mutation; the fuzz and sanitizer soak at G2 and G3; cmd behind an allow-list; crash dumps collected and turned into regression scenarios
The compact notation misleads Claude (tuplets, ties, voices, transposing instruments) and it writes the wrong music
Edits that are syntactically valid and musically wrong
Medium · Medium
Lossless round-trip tests; written pitch and sounding pitch both stated; bar-sum validation with precise error positions; the skill's read-then-verify loop; everything undoable
MuseScore 5 arrives with the framework in a submodule, the new command system and file format 5.00
A rebase that is a port, not a merge; one to three weeks of work at a time of its choosing
Certain · Medium
The score service never touches the transport; commands mirror rcommand shapes; Phase 5 starts the conversation before 5.0 ships
Monthly point releases drift under the fork
Merge conflicts in files the module touches; golden exports change
Certain · Low
New directory plus about five registration lines; sync-upstream.sh; goldens re-baselined deliberately with a diff review
Upstream declines the contribution or chooses different semantics (new product owner Oct 2026, stated 5.0 priorities are playback automation and shared staves)
The fork remains the long-term home
Medium · Low for you, High for the zero-maintenance end state
Small read-only PRs first; the fork stays healthy either way
Large transactions freeze the UI during relayout
A 200-bar batch takes seconds; the window is unresponsive meanwhile
Medium · Low
One transaction per musical task; progress notifications; chunking for very large batches; latency budgets measured in the log
Toolchain friction on macOS: Qt 6.10.2 install, Xcode versions, Gatekeeper on an unsigned app, 20 to 25 GB of disk
Days lost before the first build
Medium · Low
Phase 0 exists to absorb this once; everything scripted; ad-hoc code signing is enough for a local app
A local HTTP server inside a GUI app widens the attack surface
Any local process could edit scores
Low · Medium
Loopback bind, origin check (already in the upstream code), bearer token, an off switch in Preferences, no commands that touch files outside the temp and backup folders
Re-entrancy: a second request arrives while a commit is mid-layout, or during one of MuseScore's own progress dialogs
Corrupted undo state, a crash
Low · High
One in-flight request at a time (E_BUSY); refusal while modal or progress dialogs are open
Timeouts over the longest chain (claude.ai session → desktop app → your Mac)
Calls that succeed in MuseScore but report failure
Medium on that chain · Low
Claude Code locally for serious sessions; every call bounded; progress for long ones; stateVersion lets a retry detect that the work already happened
Data loss from a committed bad batch followed by a save
Good work overwritten
Low · High
Backup snapshot before multi-bar batches; nothing saves unless asked; undo always available; score_validate exposes MuseScore's corruption check
Scope creep: 40 operations become 100
The plan slips and the test suite thins
Medium · Medium
The registry is the specification; set_property absorbs the long tail; gates do not move
Every open score is its own process, so the server exists once per score and only one can hold port 2212
Claude edits the wrong score, or cannot reach the one you are looking at
Certain · Medium
Per-instance discovery records, an optional instance argument on every command, stateless forwarding by the port-2212 holder, and every response naming the score it acted on
One thing no bridge fixes: Claude's musical judgement. A reharmonisation can be competent and still not what you wanted. The plan treats that as a workflow matter (read, propose, apply as one undo step, verify, undo if needed) rather than pretending a better API solves it.
Decisions needed from you
Six answers unblock Phase 0. None of them changes the architecture; they fix the target and the first fixtures.
[ ] Which MuseScore do you run, exactly? The plan targets 4.7.6 (released 26 Oct 2026). If you are still on 4.6.x or early 4.7.x, say so; the fork point moves, nothing else does.
[ ] Mac model, macOS version and free disk. A full build needs Xcode command-line tools, Qt 6.10.2 and roughly 25 GB of space for Qt plus the build tree. Apple silicon is assumed for the build-time estimates.
[ ] Daily driver or side by side? You can run the agent build as your only MuseScore (same settings, plugins and scores) or keep the official one installed next to it under a different app name. Side by side is safer for the first month (a dev-mode build keeps separate settings, so the two do not interfere); after that the agent build is the one you would use.
[ ] Where do you talk to Claude? Claude Code in a terminal is the shortest path (direct HTTP, image viewing for verification, and it can rebuild MuseScore for you). The desktop app works through a small stdio bridge. A claude.ai session reaching your Mac through the desktop app's device link is the longest chain and the one most exposed to timeouts; it should be the exception.
[ ] Which of your scores can be test fixtures? Ten to twenty real pieces of varied size and instrumentation (including anything with multiple voices, tuplets, cross-staff writing, linked parts and chord symbols) become the regression corpus. They stay on your machine; nothing in the plan uploads them.
[ ] Keep mcp-score around for Dorico? The new module replaces it for MuseScore only. If you ever use Dorico, mcp-score's Dorico bridge can stay installed and is unaffected.
Two things to be aware of rather than decide: MuseScore is GPL-3.0, so a private fork for your own use carries no obligations, but distributing the build to others means publishing the source; and MuseScore's AI contribution policy requires that anything proposed upstream is reviewed and understood by the human submitting it and disclosed as AI-assisted, which shapes how the Phase 5 pull requests are prepared.
Sources
Source code read directly (sparse checkouts made on 28 Oct 2026):
• musescore/MuseScore, tag v4.7.6: src/notation/inotation.h, src/notation/internal/inotationundostack.h, src/notation/inotationelements.h, src/notation/inotationinteraction.h, src/notation/inotationparts.h, src/notation/internal/notationinteraction.cpp, src/notation/notationtypes.h, src/engraving/dom/score.h, src/engraving/dom/property.cpp, src/engraving/editing/cmd.cpp, src/engraving/infrastructure/eid.h and eidregister.cpp, src/engraving/rw/write/twrite.cpp, src/engraving/types/constants.h, src/engraving/api/v1 (qmlpluginapi.cpp, score.cpp, cursor.cpp, scoreelement.h), src/framework/extensions/internal/extensionsprovider.cpp, src/framework/extensions/api/extapi.h, src/framework/extensions/qml/Muse/Extensions/internal/EnablePanel.qml, src/framework/network/networkmodule.cpp and api/websocketserverapi.cpp, src/framework/multiwindows/multiwindowsmodule.cpp and internal/multiprocess/multiprocessprovider.cpp, src/framework/musesampler/internal/musesamplerconfiguration.cpp, src/project/inotationwriter.h, the import-export module registrations, src/converter/internal/compat/backendapi.cpp, src/app/internal/commandlineparser.cpp, buildscripts/cmake/SetupQt6.cmake, buildscripts/ci/macos/build.sh and install_qt.sh, ninja_build.sh, version.cmake.
• musescore/MuseScore, master at commit 38f00e3 (27 Oct 2026): version.cmake, .gitmodules, src/app/appfactory.cpp, src/notationscene/internal/notationcommandsregister.cpp, src/notationscene/internal/notationactioncontroller.cpp, src/engraving/types/constants.h, AI_POLICY.md.
• musescore/muse_framework, commit ecdc6e7 (27 Oct 2026): framework/rcontrol/ (module, mcpserver.cpp, mcpcontroller.cpp, transport/httptransport.cpp, transport/tcptransport.cpp, bridge/main.cpp) and framework/rcommand/ (commandtypes.h, icommanddispatcher.h, commandable.h).
Pages opened:
• muse_framework pull request #377, "MCP server improvements", merged 26 Oct 2026.
• MuseScore Studio 4.7.6 release notes, 26 Oct 2026, including the cmd("add-hairpin") crash fix.
• MuseScore Studio 4.7 announcement, 13 May 2026 (Qt 6.10.2).
• MuseScore news index and GitHub releases for the 2026 release dates.
• mcp-score repository, its MuseScore plugin guide, pull request #98 (integration tests and the plugin crashes they found), issue #91 (roadmap) and the PyPI page (support matrix: 4.4.2+, tested on 4.4, 4.6 and 4.7).
• Claude Code MCP documentation: HTTP transport, --header, tool-call timeouts and output limits.
• Scoring Notes, 14 Jul 2026: Muse Group ownership and MuseScore Studio leadership changes.
• Your mcp-score-musescore-connect skill file, for how the current bridge is launched and reconnected.