# Amiga DECnet – Stack-Struktur (Skizze)

Stand: 21.10.2026. Entwurf zur Diskussion; offene Entscheidungen am Ende.

## Ziel

DECnet Phase IV **Endnode** für AmigaOS über SANA-II-Ethernet. Erster
Meilenstein jenseits des Netzes: FotoFix-Bild per FAL von FOTOFX holen.

## Prozessmodell

```
  Anwendungen                      AmigaDOS
  ┌────────┬────────┬─────────┐    ┌───────────┐
  │ dncopy │ dndir  │ FotoFix │    │ DN:-Handler│
  └───┬────┴───┬────┴────┬────┘    └─────┬─────┘
      │  decnet.library (API, Session Control-Fassade)
      └─────────────┬──────────────────────┘
                    │ Messages
            ┌───────▼─────────────────────────────┐
            │ DECnet-Prozess ("dnetd")            │
            │  ┌───────────────────────────────┐  │
            │  │ Objekte im Stack: MIRROR (25) │  │
            │  ├───────────────────────────────┤  │
            │  │ Session Control               │  │  portabler
            │  │ NSP (Logical Links)           │  │  Kern (ANSI C,
            │  │ Routing (Endnode)             │  │  ohne AmigaOS)
            │  ├───────────────────────────────┤  │
            │  │ Datalink-Adapter SANA-II      │  │  Amiga-spezifisch
            │  └───────────────────────────────┘  │
            └───────┬─────────────────────────────┘
                    │ IORequests
             uaenet.device / a2065.device / …
```

* **Ein Stack-Prozess** besitzt das SANA-II-Device, alle Timer (Hellos,
  NSP-Retransmits, Inaktivität) und den gesamten Protokollzustand. Kein
  Locking nötig, weil nur er den Zustand anfasst.
* **Anwendungen** sprechen ihn über eine **Shared Library** an
  (`decnet.library`); die Library-Funktionen schicken Messages an den
  Prozess und warten (oder liefern Signale für asynchrone Nutzung). Das ist
  das Muster von AmiTCP/Roadshow (`bsdsocket.library`) und erlaubt später
  auch ARexx- oder andere Sprach-Anbindungen.
* Der **DN:-Handler** (wie dnprogs `dapfs`) ist nur ein weiterer Client der
  Library.

## Portabler Kern

Routing, NSP und Session Control werden als **plattformneutrales C** ohne
AmigaOS-Aufrufe geschrieben. Die Schnittstelle zur Umgebung ist schmal:

* `frame_send(dst, type, buf, len)` / `core_frame_received(...)`
* `core_tick(ms)` (Zeitbasis) und eine Uhr
* Puffer-Allokation

Vorteil: Derselbe Kern läuft **auch auf dem Pi unter Linux**:

1. **Unit-Tests und Replay** der vorhandenen Mitschnitte
   (`captures/*.pcap`, Mac-Projekt `~/Developer/Mac/DECnet/captures/`).
2. **Live gegen HOWIE** über `AF_PACKET` auf `br0` mit einer eigenen
   Testadresse – schnelle Iteration ohne Emulator, Cross-Compiler und
   Tipp-Skripte. Erst wenn eine Schicht dort sitzt, geht sie auf den Amiga.

## Schichten

### Datalink (Amiga)
* SANA-II öffnen, `S2_CONFIGINTERFACE` mit `AA-00-04-00-xx-xx`,
  `S2_ADDMULTICASTADDRESS` für `AB-00-00-04-00-00` (all endnodes).
* Lesen: `CMD_READ` Typ 0x6003 (später 0x6002 MOP); Schreiben: `CMD_WRITE`.
* **Fallback** für Devices, die die Adresse ignorieren (unpatched
  uaenet.device): Promiscuous öffnen, RAW lesen/schreiben, selbst filtern.
* Immer `BeginIO()`, nie `SendIO()` (SendIO löscht `SANA2IOF_RAW`).

### Routing (Endnode, Routing-Spec 2.0.0)
* Endnode-Hellos (wie `dnhello`), designierter Router, Listener-Timer.
* Datenpakete: Long-Header auf Ethernet senden, Short/Long empfangen,
  Padding, „Return to Sender“.
* **On-Ethernet-Cache**: Pakete mit Intra-Ethernet-Bit von Knoten X → X
  danach direkt adressieren statt über EUGENE (so macht es VMS).

### NSP (4.1)
* Logical Links: Connect Initiate/Confirm/Reject, Disconnect, Link Service.
* Normal- und Interrupt-Subkanal, Segmentnummern mod 4096, Ack/Nak
  (auch huckepack), Flusskontrolle (none/segment/message), Retransmit mit
  geschätzter Laufzeit (Delay-Faktor), Inaktivitätstimer, Segmentierung
  nach Segsize (FOTOFX: 563).

### Session Control
* Objekte nach Nummer und Name, Zugangsdaten (User/Passwort/Account) bei
  Connect, Ablehnungsgründe (z. B. 34 = Access denied).
* Knotennamen-Datenbank: einfache Textdatei `ENVARC:DECnet/nodes`
  (`HOWIE 23.208` …); numerische Adressen gehen immer.
* Konfiguration `ENVARC:DECnet/config`: Adresse, Name, Device, Unit.

### Objekte / Anwendungen (Reihenfolge = Meilensteine)
1. **MIRROR (Objekt 25)** im Stack → `NCP LOOP NODE 23.202` von HOWIE.
2. **DAP-Client** (aus `dapget` des Mac-Projekts, eigener C-Code) →
   `dncopy`/`dndir`, dann FotoFix (FOTOFX: `blocking=no`, siehe Bit-18-Befund).
3. **DN:-Handler** (`DN:NODE"user password"::SYS$LOGIN:` o. ä.).
4. Eingehende Dienste: FAL-Server (17), CTERM (42), NML (19) für
   `TELL AMI SHOW EXECUTOR`, MOP-SysID (RetroStar zeigt `mop-rc`).

### DN:-Handler: Lesen und Schreiben (Stand 22.10.2026)
* `DN:` = Knoten aus `ENV:DECnet/nodes`, `DN:HOWIE/…` = Login-Verzeichnis.
  Lesen, Schreiben (neue Datei bzw. neue VMS-Version), Löschen (alle
  Versionen, `;*`) und Umbenennen gehen aus Shell und Workbench.
* **Text oder binär** entscheidet der erste `Write()`: ohne Steuerzeichen →
  VAR-Records mit CR-Attribut (auf VMS mit `TYPE` lesbar, Zeilen = Records);
  sonst VAR-Records à 1024 Byte ohne Attribut (byte-genau zurück). VMS
  zählt bei VAR die Längenwörter mit, Listings zeigen daher ~0,2 % mehr;
  `Seek(OFFSET_END)` zählt bei VAR die Daten nach.
* **Namen**: VMS (ODS-2) erlaubt nur `A-Z 0-9 $ _ -` und einen Punkt. Alles
  andere (weitere Punkte, Leerzeichen, Umlaute, `$`) wird als `$xx` (hex)
  gespeichert, z. B. `Bild.iff.info` → `BILD$2EIFF.INFO`, und beim Listen
  zurückübersetzt. `README.` (leerer Typ) erscheint als `README`.
* **Knotendatei** `ENV:DECnet/nodes`: `NAME ADRESSE [USER name] [PASSWORD
  wort] [NOBLOCK] [PURGE] [SHOW]`, Schlüsselwörter in beliebiger Reihenfolge
  (bis 10 Wörter pro Zeile). `USER`/`PASSWORD` stehen als Schlüsselwort vor
  dem Wert, damit ein Benutzer oder Passwort „SHOW“ nicht als Option gilt
  (Entscheidung 22.10.2026; das alte positionelle Format mit „-“ entfällt).
* **SHOW**: Nur so markierte Knoten erscheinen als Schubladen in `DN:`; hat
  kein Knoten SHOW, erscheinen alle. Andere bleiben per Name erreichbar
  (`DN:COLT`).
* **VMS-Versionen**: Schreiben legt eine neue Version an; `DN:` zeigt nur die
  neueste. Mit `PURGE` hinter dem Knoten in `ENV:DECnet/nodes` löscht der
  Handler danach die älteren (`NAME;-1` bis „nicht gefunden“), ohne
  bleiben sie liegen (Voreinstellung, Entscheidung 22.10.2026).
* **Belegung** (`ACTION_INFO`): fest „0 % voll, 512 M frei“. DAP kann den
  freien Platz nicht erfragen. Die Workbench zeigt die Zeile in jedem
  Laufwerksfenster (Schubladen nie) und kopiert nicht, wenn die Abfrage
  abgelehnt wird; „0 frei“ ließe andere Programme abbrechen. Echte Werte
  gingen nur per Task-to-Task mit einer DCL-Prozedur auf dem VMS-Knoten
  (`F$GETDVI(..,"FREEBLOCKS")`, getestet mit `dntool task`, HOWIE: 1666836
  von 2050860 Blöcken frei) – verworfen, 22.10.2026.
* Schutzbits, Kommentar und Datum werden angenommen und verworfen (sonst
  brechen Workbench-Kopien ab). Verzeichnisse anlegen kann DAP nicht.
* Das Laufwerk heißt **„DECnet Nodes“**, nicht „DECnet“: ein Volume namens
  DECnet überdeckt die Zuweisung `DECnet:` (Projekt- bzw. Installationsordner),
  dann landet z. B. `DECnet:config/nodes` beim Handler („Can't cancel DECnet“).
* Klienten-Pakete kommen an einem **eigenen MsgPort**, nicht am
  `pr_MsgPort`: Die eigenen DOS-Aufrufe des Handlers (ENV:, Log) bekommen
  ihre Antworten dort, ein dazwischen eintreffendes Klienten-Paket führte
  zum Absturz `AN_AsyncPkt` (87000004), sobald die Workbench parallel
  zugriff.
* `dnetd` lässt einen `DnWrite()` warten, solange mehr als 16 Segmente
  unbestätigt sind (`WRITE_WINDOW`, max. 60 s), damit beim Schreiben großer
  Dateien nicht die ganze Datei im Speicher von `dnetd` landet.

### Installer (Stand 23.10.2026)

Paket bauen: `tools/mkdist.sh` → `dist/DECnet/` (die Schublade, die der
Benutzer auspackt) und `dist/DECnet.lha`. Quellen in `installer/`
(`Install-DECnet`, `ReadMe`, `S/DECnet-Start.body`, `S/DECnet-Stop`,
`Storage/Mountlist`, `Storage/nodes.head`). Test im Emulator auf einer
Kopie der Festplatte: `CONFIG=~/Amiberry/Configurations/DECnet-InstTest.uae
./amiberry_decnet.sh` (Hardfile `A3000_HD00 InstTest.hda`).

* **Ziel-Layout**: `<Schublade>/DECnet/{C,L,S,Storage}`, `LIBS:decnet.library`,
  `ENVARC:DECnet/nodes` (wird nie überschrieben), Gerät und Unit als
  `ENVARC:DECnet/{Device,Unit,Address,Name}` (Vorgaben fürs nächste Update,
  per `getenv`). **Eine Neuinstallation gibt Adresse und Knotennamen nie vor**
  (Entscheidung Torsten, 23.10.2026), damit kein unbedachter Klick eine
  Adresse doppelt ins Netz bringt. Nur beim Update (Zuweisung `DECnet:`
  vorhanden) stehen die bisherigen Werte als Vorgabe da (Wunsch Torsten,
  24.10.2026). Im Paket stehen auch keine echten Adressen oder Knotennamen
  als Beispiele. `DECnet-Start` schreibt der Installer: `.def`-Zeilen +
  `S/DECnet-Start.body`. Ein Update erkennt der Installer an der Zuweisung `DECnet:`.
* **Start vor TCP/IP**: `C/dnstartup DIR=x` setzt den Block
  `;BEGIN DECnet … ;END DECnet` vor die erste Zeile mit AmiTCP/startnet/
  AddNetInterface/Roadshow/Miami/Genesis, und zwar vor den *ganzen*
  Installer-Block, der sie enthält (auf der Testplatte z. B. vor `;BEGIN Miami`). Sonst kommt er ans Ende.
  `dnstartup REMOVE` entfernt den Block, die alte Datei bleibt als `.bak` erhalten. Netzsoftware
  in der Startup-Sequence vor der User-Startup → RC 5 + Warnung.
  (Das Installer-eigene `(startup)` kann nur anhängen und nicht entfernen.)
* **dncheck** läuft mit Ausgabe in `CON:…/CLOSE`. `(run)` wartet nur
  auf das Programm, nicht auf das Fenster; deshalb liest dncheck 0.5 mit
  CLOSEHINT am Ende vom Ausgabe-Handle (CON: beantwortet das), bis das
  Schließgadget EOF liefert. Erst dann geht der Installer weiter. Läuft dnetd schon, überspringt dncheck
  den Empfangstest (dnetd nimmt die Pakete selbst; bei uaenet gibt es nur eine
  Lese-Queue).
* **Einsteiger-Modus** beantwortet alle Fragen mit der Vorgabe → Endlosschleife
  bei der Adresse. Deshalb bricht das Skript bei `@user-level` 0 ab, das Icon bekommt
  `MINUSER=AVERAGE`, und aus der Shell heißt es
  `Installer Install-DECnet MINUSER AVERAGE`.
* **LHA**: Lhasa kann nur auspacken. Gepackt wird mit LHa for UNIX
  (`~/Developer/toolchains/lha-src`), lh5, *generic level 0*, nur Dateien.
  `tools/lha_amigapath.py` ersetzt den Pfadtrenner 0xFF durch `/`
  (sonst entpackt LhA 1.10 von 1991 nichts) und korrigiert die Header-Prüfsumme.
* **Deinstallation**: stoppt dnetd, `dnstartup REMOVE`, löscht Schublade und
  Library, behält `ENVARC:DECnet`. Der Ordner `C` bleibt leer stehen, solange der
  Shell-Pfad `DECnet:C` ihn sperrt (bis zum Neustart); das meldet der Installer.
* Verifiziert 23.10.2026 (Installer 44.10, OS 3.1, Amiberry): Neuinstallation,
  Update, Deinstallation, Boot über User-Startup, Entpacken mit LhA 1.10,
  `dnnodes FROM=23.21 REACHABLE UPDATE` aus dem Installer, DN: danach nutzbar.
* **Icons** (23.10.2026, Wunsch Torsten: klassisch + GlowIcons, Knoten = VAX-Schrank,
  Volume = Laufwerk mit DEC-Logo-Schriftzug, Texte nur Englisch): `tools/icons.py`
  zeichnet alle Icons mit Farbrollen und erzeugt daraus das 4-Farben-Bild und die
  GlowIcon-Bilder (FORM ICON, RLE). `tools/mkicon.py` schreibt die `.info`-Dateien;
  das Format wurde an den Systemicons geprüft (Decoder, bitgenauer Rundlauf).
  GlowIcon-Farben liegen nahe an den Stiften einer 16-Farben-Workbench (WB-Blau,
  Creme/Beige, Rot, Grün), sonst verfälscht die Umrechnung sie (Dunkelblau → Grün).
  Ein Fehler, den ich dabei behoben habe: Palettenindex 0 (transparent) darf beim
  Nachschlagen nicht Schwarz treffen, sonst fehlen alle Umrisse.
* **dn-handler 0.3**: liefert `DN:Disk.info` und `DN:<KNOTEN>.info` aus
  `DECnet:Icons/{Disk,Node}.info` (einmal geladen, aus dem Speicher gelesen); die
  Wurzel listet zu jedem Knoten `KNOTEN.info`, so zeigt die Workbench die Knoten
  ohne „Alle Dateien“. Schreiben in die Wurzel (z. B. Icon-Position speichern) →
  `ERROR_DISK_WRITE_PROTECTED`. Ohne `DECnet:Icons` gelten die Workbench-Vorgaben.
* **dn-handler 0.4 – virtuelle Icons in den Knoten**: Jede Verzeichnisliste
  bekommt zu jedem Eintrag ein `NAME.info` aus `DECnet:Icons/`: `Dir` (Ordner), `Text`
  (.TXT .COM .LIS .LOG .C …, MultiView), `Picture` (.IFF .GIF .JPG …, MultiView),
  sonst `File`. Ein echtes `.info` auf dem Knoten hat Vorrang. Löschen/Umbenennen
  eines virtuellen Icons wird ohne Wirkung bestätigt.
* **Keine `.info`-Dateien auf den VMS-Rechnern** (Vorgabe Torsten, 23.10.2026):
  `.info`-Schreibzugriffe der Workbench (Position speichern, „Informationen“, Icon
  einer gezogenen/verschobenen Datei) und von Programmen ohne CLI werden
  angenommen und verworfen. Ein `.info`, das ein Befehl schreibt (Shell, Skript,
  `Run`, „Befehl ausführen“), landet wirklich auf dem Knoten.
* **dn-handler 0.5 – Workbench richtig erkennen**: Die Workbench von OS 3.1 läuft
  selbst als Hintergrund-CLI (von `LoadWB`): Task „Workbench“, `cli_Background`
  -1, `cli_CommandName` „Workbench“. Ihr Verschieben einer Datei (Rename, danach
  `PutDiskObject` am Ziel, weil das virtuelle Icon an der Quelle mit der Datei
  verschwunden ist) schrieb mit 0.4 deshalb doch ein `.info`. `from_shell()`
  verlangt jetzt eine CLI **und** weder Task- noch Befehlsname „Workbench“.
  Gemessen (Log): Shell = „Shell Process“/bg 0, `Run` und „Befehl ausführen“ =
  „Background CLI“/bg -1, jeweils Befehl „Copy“. Außerdem: Umbenennen auf ein
  Ziel, an dem nur ein virtuelles Icon steht, ist erlaubt (sonst scheiterte das
  Verschieben einer Datei mit echtem Icon mit 203 „Objekt existiert bereits“).
  Debug-Log: `ENV:DECnet/handler-debug` mit Dateinamen anlegen; jedes Paket und
  der Absender von `.info`-Schreibzugriffen werden protokolliert.
* **Workbench-Tests auf HOWIE (23.10.2026, 0.5)**: Schublade für `[.DNTEST]` mit
  Dir-Icon, öffnen, Text-Icon darin; Fixieren, Löschen, Umbenennen über das
  Menü; Datei mit virtuellem Icon verschieben (kein `.info` auf HOWIE); Datei mit
  echtem Icon verschieben (Icon zieht mit); Shell-Kopie eines `.info` bleibt.
  Verzeichnisse anlegen/löschen kann DN: nicht (`CREATE_DIR` gibt es in DAP
  nicht; VMS legt `.DIR` ohne Löschrecht an). Für Tests: DCL-Prozedur per DN:
  hinkopieren und mit `dntool task HOWIE NAME` starten (Timeout ist normal).

* **Test A2000 / OS 2.04 / 68000 (23.10.2026)**: Amiberry-Konfiguration
  `DECnet-A2000-204.uae` (Kickstart 37.175, ECS, 1 MB Chip + 4 MB Fast, 68000
  `cpu_speed=real`, uaenet unit 2 = tap0), Platte `A2000_HD00 OS204.hdf` (64 MB,
  ohne RDB, FFS), Workbench 2.04 von den vier ADFs kopiert, deutsche Tastatur
  über `ENVARC:sys/keyboard`. Installer 43.3 und LhA 2.15 von Aminet
  (`util/misc/Installer-43_3.lha`, `util/arc/lha.run`); das LhA der 3.1-Platte
  braucht `arp.library`. Ergebnis: Archiv entpackt, Icons (klassisch) in Ordnung,
  Installer 43.3 läuft (MINUSER greift), dncheck RC 0 mit Router-Hello,
  `User-Startup` neu angelegt, Boot startet dnetd und DN:, dnnodes von 23.21,
  DN:HOWIE mit virtuellen Icons, TEST.TXT per Doppelklick in More.
  Gefundene und behobene Fehler:
  - `Mount` 37.2 kennt `Activate` nicht (erst 3.0) → Mountlist `Mount = 1`
    (3.x versteht beides). Das gescheiterte Mount brach `DECnet-Start` ab, bevor
    dnetd startete → dnetd startet jetzt vor dem Mount.
  - `Path DECnet:C ADD` lief nur einmal pro Boot (an `setup-done` gebunden), der
    Pfad gehört aber der ausführenden Shell: nach „Start now“ im Installer fand
    eine spätere Shell `dnetd` nicht, `Run >NIL: dnetd` scheiterte stumm → jetzt
    `Which >NIL: dnetd` / `If WARN` / `Path … ADD` bei jedem Lauf (Which gibt
    unter 2.04 RC 5).
  - `Mount` 37.2 öffnet „DN:“ selbst mit MODE_NEWFILE, um den Handler zu
    starten → „Volume DECnet Nodes is write protected“-Requester beim Booten.
    dn-handler 0.5 antwortet für die Wurzel selbst mit `ERROR_OBJECT_WRONG_TYPE`
    (kein Requester).
  - ReadMe-Icon im Archiv mit Standardprogramm MultiView ging unter 2.04 nicht →
    „More“ (ohne Pfad, gibt es überall); der Installer setzt ab OS 3.0 MultiView.
  Bild-Icons haben unter 2.04 kein Anzeigeprogramm (steht in der ReadMe).

* **Test A1200 / OS 3.2.3 (23.10.2026)**: Konfiguration `DECnet-A1200-323.uae`
  (AGA, 68020, Kopie der MiSTer-Platte `A1200_HD OS323 InstTest.hdf`, WHDLoad
  nur lesend, Transfer: statt NAS:, uaenet unit 2). Installer 47.19 (deutsche
  Oberfläche, unsere Texte englisch), LhA vorhanden. Ergebnis: GlowIcons von
  Archiv, Volume, Knoten und Dateien sauber; dncheck RC 0; DECnet-Block landet
  vor dem `;BEGIN Miami`-Block; dnnodes; Boot mit DN:. OS 3.2 verlinkt
  `RAM:ENV` auf `ENVARC:` (MakeLink): Schreiben nach ENV: legt eine eigene
  Kopie an, ENVARC: bleibt unberührt — keine doppelten nodes-Zeilen, die
  Merker `setup-done`/`dn-mounted` überleben keinen Neustart. Menüs sind dort
  „sticky“ (Auswahl per Linksklick).
  **Fehler gefunden und behoben (dn-handler 0.6):** MultiView/text.datatype von
  3.2 fragt mit ACTION_EXAMINE_FH die Größe und liest genau so viele Bytes;
  DN: meldete für VMS-Textdateien (VAR) die Plattengröße (mit Längenwörtern,
  z. B. 2822 statt 2642) → „Nicht genug Daten vorhanden“. Jetzt liefert
  EXAMINE_FH die exakte Größe über `file_size()`; Textdateien bis 256 KB werden
  dabei ganz in den Speicher gelesen (`file_to_mem`, kein zweites Lesen),
  größere gezählt und neu geöffnet. Nebenbei behoben: `file_size()` setzte
  `f.text = 0` als Merker, danach fehlten beim erneuten Lesen die Zeilenenden
  (jetzt eigenes Feld `exact`). Kopie per Copy bytegenau geprüft.

### Echter A3000: Befunde und Fixes (23.10.2026 abends)
Test auf dem echten A3000 (OS 3.5, AmiTCP 4.1, a2065.device Unit 0) bestanden;
Mitschnitt `captures/2026-10-23-a3000-live.pcap`. Danach behoben (noch nicht
auf echter Hardware nachgetestet):
* **a2065.device gibt einen abgebrochenen `CMD_READ` nie zurück.** `WaitIO()`
  nach `AbortIO()` hängt dann für immer (dncheck 0.2, dnetd beim Beenden).
  Endet das Programm trotzdem, ruft der Treiber beim nächsten Frame die
  Copy-Hooks im entladenen Code auf → Guru. Jetzt (`src/amiga/s2buff.h`):
  `s2_abort()` wartet höchstens ~2 s; die Hooks sind ein 18-Byte-Stub in
  AllocVec-Speicher, der nur `CopyMem` aus dem ROM aufruft. Bleibt ein Read
  beim Treiber, lassen dncheck 0.4 und dnetd 0.4 Stub, Request, Puffer, Port
  (`PA_IGNORE`) und das geöffnete Device absichtlich liegen.
* **Frames außer der Reihe.** Beim Lesen einer 270-KB-Datei von HOWIE
  verarbeitete der A3000 Segment 73 vor 72 (am Draht in richtiger
  Reihenfolge); NSP verwarf daraufhin den Rest des Bursts, VMS wiederholte
  erst nach 18,4 s. Der Treiber sortiert offenbar um, wenn alle Reads belegt
  sind. Jetzt: `NREAD` 32 statt 8, und NSP hält bis zu 16 vorauslaufende
  Segmente pro Link (Fenster 64) und liefert sie nach, sobald die Lücke
  gefüllt ist; besteht eine Lücke 200 ms, geht ein NAK hinaus (danach jede
  Sekunde), VMS wiederholt dann sofort. Host-Test: `make -C src/linux test`.
* **„Retransmit nach 64 ms“** war keiner: HOWIE schickte LS „stop“, dann
  `NAK=52` + LS „resume“; Segment 53 neu zu senden war richtig.
* **Viele FAL-Verbindungen durch die Workbench** (Icon 4×, Text 7× geöffnet,
  je ~0,5 s Verbindungsaufbau): dn-handler 0.7 liest Dateien bis 32 KB beim
  ersten Öffnen ganz und schließt den Link sofort; weitere Öffnungen innerhalb
  10 s kommen aus dem Speicher (4 Einträge), ohne Verbindung und ohne
  Listing. Schreiben, Löschen, Umbenennen verwerfen den Eintrag.
* **Nur `ENVARC:DECnet/nodes` bearbeitet** wirkte erst nach dem Neustart. Das
  ist AmigaOS-Konzept und bleibt so (Entscheidung 23.10.2026): Handler und
  Library lesen nur `ENV:`, ENVARC: ist der Vorrat für den Boot. Ein
  Zwischenstand „die neuere Datei gewinnt“ wurde wieder verworfen.
  Stattdessen erklären Dateikopf, ReadMe und Installer-Schlusstext den
  `Copy ENVARC:DECnet/nodes ENV:DECnet/nodes`. dnnodes 0.3 schreibt bei UPDATE
  in beide Dateien und prüft ENVARC: gegen den eigenen Inhalt (keine
  doppelten Zeilen). Nebenbefund: `DnNodeAddr()` las nur die ersten 4096 Bytes
  der Datei; decnet.library 1.1 liest zeilenweise.
* **decnet.library 1.1 ließ sich nicht laden** (gefunden 23.10.2026 spät in
  Amiberry, vor dem Nachtest auf dem A3000): `OpenLibrary()` scheiterte,
  `Version` las die Datei trotzdem. Die kürzere `DnNodeAddr()` hatte das
  Romtag auf 0x56e verschoben (1.0: 0x5b0); ramlib findet nur ein auf ein
  Langwort ausgerichtetes Romtag. Jetzt `__attribute__((aligned(4)))`; gegen
  die unausgerichtete Fassung per A/B-Test bestätigt. Das Paket aus 770c637
  (95379 Bytes) ist damit unbrauchbar.

### Nachtest auf dem A3000 (24.10.2026)

* **HOWIE**: 102 KB per Workbench in 1,2 s, per `dncopy … PROGRESS=WINDOW`
  in knapp 1 s, ohne Wiederholung (vorher mit dnetd 0.3: 19 s Hänger nach
  einem verlorenen Segment). Für einmal Öffnen + Ziehen 4 statt 7
  FAL-Verbindungen. FOTOFX: ADF mit 90 KB/s.
* **FOTOFX-Verzeichnis ohne Blocking** kommt als ein winziges Segment pro
  Eintrag (30–60 Bytes) in Salven zu 20, praktisch ohne Abstand. Davon
  verliert der a2065 einige und vertauscht manchmal eins. FOTOFX ignoriert
  NAKs, wiederholt nach ~2 s und schickt dabei zuerst die *letzte* Salve
  noch einmal, erst später die fehlenden Segmente. Das füllte den Cache
  (16) mit späten Segmenten; die Lückenfüller wurden dann abgewiesen, FOTOFX
  schickte sie nie wieder und gab den Link auf (dndir-Timeout). dnetd 0.5:
  Cache 32, und bei vollem Cache weicht das am weitesten vorauslaufende
  Segment einem näheren (nsptest-Fall). `dndir FOTOFX MUSTER/* NOBLOCK`:
  2 min 19 s / Timeout → 24 s. **Mit Blocking** packt FOTOFX die Einträge in
  26 große Segmente: kein Verlust, ~7 s. Der FOTOFX-Fehler mit Blocking
  betrifft nur ACCESS auf fehlende Dateien; NOBLOCK als Vorgabe für FOTOFX
  ist daher zu überdenken.
* **NOBLOCK nur noch für Dateizugriffe** (dn-handler 0.8, dndir 0.3). Test
  vom Pi gegen FOTOFX (`dntool dir`/`get`, je mit und ohne Blocking):
  Verzeichnisanfragen auf fehlende Datei, fehlenden Ordner oder leere
  Wurzel beantwortet FOTOFX auch mit Blocking sofort richtig; `MUSTER/*`
  dauert mit Blocking 0,6 s statt 3,3 s. Nur *Öffnen* einer fehlenden Datei
  oder in einem fehlenden Ordner bleibt mit Blocking ohne Antwort (Timeout),
  der Fehler besteht also weiter. Deshalb bieten Links, die nur Verzeichnisse
  lesen (Auflisten, Einzelprüfung bei Lock/Examine), immer Blocking an;
  Öffnen, Schreiben, Löschen, Umbenennen und dncopy beachten NOBLOCK. dndir
  behält NOBLOCK in der Vorlage, wirkungslos.
* **Virtuelle Icons nur für die Workbench** (dn-handler 0.9). `Copy
  DN:HOWIE/getandplay#? Work:slot` brachte eine `.info` mit, die es auf HOWIE
  nicht gibt: die Verzeichnisliste (Cache, für alle Aufrufer gleich) enthält
  die virtuellen Icons. ExNext liefert sie jetzt nur noch dem Prozess
  „Workbench“ (`sender()`: Taskname oder CLI-Befehlsname); Shell-Befehle und
  Programme ohne CLI wie Directory Opus 4.12 (zeigte und kopierte sie) sehen
  sie nicht. Nach `NAME.info` gezielt fragen geht für alle; echte `.info`
  auf dem Knoten bleiben sichtbar. Icon-Schreibzugriffe verwirft der Handler
  weiter für alles außer Shell-Befehlen.
* **NSP-Flusssteuerung: vorerst nicht** (Entscheidung 24.10.2026, nach dem
  A2000-Test neu bewerten). Dateiübertragungen von FOTOFX auf dem A3000:
  1764/1768/1767 Segmente mit 2/1/2 Wiederholungen (0,1 %), keine NAKs;
  HOWIE 102 KB ohne Wiederholung. Große Segmente (563–1490 Bytes) haben am
  Draht genug Abstand für den a2065; Verluste gab es nur bei Salven
  winziger Segmente (Verzeichnis ohne Blocking: 149 Segmente, 39 %
  wiederholt), und die fallen mit dn-handler 0.8/dndir 0.3 weg. Kosten
  dagegen: Segment-Flusssteuerung im CI/CC verlangen und laufend
  Gutschriften per Link Service schicken; ein Fehler legt den Link still,
  und ob FOTOFX sie beachtet, ist offen (NAKs ignoriert es). Auf dem echten
  A2000 (68000) kann der Empfang auch große Salven nicht mehr schaffen —
  dann mit Messwerten entscheiden.
* **a2065.device hält einzelne Frames fest.** AMI wiederholte gelegentlich
  ein Disconnect Initiate (3 von ~30 Verbindungen), obwohl HOWIE es nach
  7–13 ms bestätigt hatte; die Wiederholung nach 1 s beantwortete HOWIE mit
  „no link“. Abgleich Mitschnitt ↔ `T:dnetd.log` (Versatz aus fünf
  Ereignissen, Streuung < 20 ms): dnetd verarbeitete das *erste* DC
  („reason 42“) erst eine Sekunde später, im Moment des zweiten DC; das
  zweite fand keinen Link mehr. Der Frame war also nicht verloren, sondern
  lag im Treiber. dnetd holt bei jedem Aufwachen (mindestens jeder Tick)
  alle fertigen Reads ab, an der Empfangsschleife liegt es nicht. Typisches
  LANCE-Treiber-Rennen: Ein Frame, der zwischen Abarbeiten des Rings und
  Quittieren des Interrupts fertig wird, bleibt bis zum nächsten Interrupt
  liegen. Erklärt auch Segment 62 nach 63–77 (FOTOFX) und 73 vor 72 (HOWIE,
  23.10.). Ob erst der nächste empfangene Frame oder schon unser eigener
  Sende-Interrupt den Frame freigibt, trennt die Log-Auflösung (20 ms)
  nicht. Folgen: beim Disconnect harmlos; bei Daten kann ein festgehaltenes
  *letztes* Segment einer Salve bis zum Retransmit-Timeout des Senders
  liegen (am 24.10. nicht als Hänger beobachtet). Möglicher Workaround, falls
  es stört: wartet dnetd auf ein DC oder ein Lückensegment, nach ~50 ms einen
  harmlosen Frame senden (z. B. ein zusätzliches Endnode-Hello), um den
  Treiber anzustoßen — erst am A3000 prüfen, ob Senden überhaupt freigibt.
  Gehört in die Treiber-Kompatibilitätsliste: a2065.device 2.16 auf der
  Ameristar-Karte (MAC 00-00-9F-…) im A3000.

### dncopy PROGRESS (Stand 23.10.2026)
* `PROGRESS=WINDOW`: Fenster auf dem Workbench-Schirm (nur Intuition/Graphics
  V37, keine GadTools) mit Knoten samt Adresse, Datei, Schritt, Balken,
  Bytezähler und KB/s, Vorbild DAPGet auf dem Mac. Bleibt am Ende 1,5 s
  stehen, mit Fehler 4 s (Text umbrochen). `PROGRESS=TEXT`: eine Shell-Zeile
  mit `\r`, höchstens 5 Aktualisierungen/s. Für die Ausstellung und den
  Amiga-FotoFix.
* Größe aus dem ATTRIB des Servers; ohne sie zählt nur der Bytezähler, das
  Fenster zeigt ein Lauflicht. Am Ende gilt die tatsächliche Byte-Anzahl als
  100 %.
* dncopy liest jetzt schrittweise (`dap_open_read()`/`dap_read_record()`)
  und wickelt `d->read` in einen Wrapper, der in 250-ms-Stücken wartet: so
  zählt die Anzeige ab 2 s Wartezeit die Sekunden, und Ctrl-C bzw. das
  Schließsymbol wirken auch, solange der Server schweigt. Nach einem Abbruch
  wird der Link nur getrennt; ein ordentliches ACCOMP-Close müsste erst den
  Rest der Datei empfangen (bei FOTOFX ohne Blocking ~15 s).
* Getestet in Amiberry gegen FOTOFX: 92 KB, 397 KB, 719 KB (~97 KB/s),
  nicht vorhandene Datei, Abbruch in beiden Modi, falscher Wert.

### Zugangsdaten aus nodes für alle Tools (Stand 23.10.2026)
* Bis dndir 0.1, dncopy 0.2 und dnnodes 0.3 kannten nur DN: die Angaben
  USER/PASSWORD/NOBLOCK aus `ENV:DECnet/nodes`; die Tools brauchten sie auf
  der Befehlszeile (Passwort sichtbar, z. B. in `Status`).
* Der Parser steckt jetzt in `src/common/nodecfg.c` (`nc_lookup()`, dazu
  `nc_ieq`, `nc_parse_addr`, `nc_word`) und wird von dn-handler,
  `dapamiga.c` (dndir, dncopy) und dnnodes gemeinsam benutzt; keine neue
  Funktion in decnet.library.
* Vorrang: USER auf der Befehlszeile bringt sein PASSWORD mit; ohne USER
  gelten USER und PASSWORD aus nodes (ein PASSWORD allein auf der
  Befehlszeile ersetzt nur das Passwort). NOBLOCK gilt, wenn es an einer
  der beiden Stellen steht.
* PURGE bleibt beim Handler: dndir und dncopy schreiben nicht. Kommt ein
  Upload in dncopy, bekommt es `PURGE/S` nach demselben Muster.
* Getestet in Amiberry: `dndir HOWIE`, `dncopy HOWIE …` ohne Zugangsdaten,
  `dncopy FOTOFX …` ohne NOBLOCK-Schalter, falscher USER auf der
  Befehlszeile → „access denied“, `dnnodes FROM=HOWIE`, DN: (Wurzel mit
  SHOW, Listing, Kopieren).

### Knotennamen per NML (Stand 22.10.2026)
* DECnet Phase IV hat keinen Namensdienst, Hellos tragen keine Namen.
  `dnnodes FROM=<knoten> [UPDATE] [FILE=…]` liest wie `NCP COPY KNOWN NODES`
  die volatile Knotendatenbank eines anderen Knotens über NML (Objekt 19,
  NICE „read information“, Option 0 = Summary, Entity KNOWN = 0xFF).
  Antwort: `2` (es folgen mehrere), je Knoten `1`, Detail (2), Fehlertext,
  Adresse (2, LE), Name (Längenbyte, Bit 7 = Executor), Parameter; Ende `-128`.
* Portabler Client `src/nml/nml.c` (wie `dap.c`), Test unter Linux mit
  `dntool nodes NODE [permanent]`. Die permanente Datenbank (`LIST`) braucht
  auf VMS Rechte (EUGENE: `NML-E-OPENIN … NETNODE_LOCAL.DAT`).
* Die Liste eines Knotens enthält alle dort **definierten** Namen, auch
  längst abgeschaltete (EUGENE: 15 von 19 „unreachable“). Der Zustand steht
  als Parameter 0 (C-1: 4 = reachable, 5 = unreachable) hinter dem Namen;
  `nml.c` überspringt die übrigen Parameter anhand ihres Datentyp-Bytes.
  `dnnodes` zeigt ihn an, `REACHABLE` übernimmt nur erreichbare Knoten (der
  Quellknoten selbst zählt immer als erreichbar).
* `UPDATE` hängt nur Knoten an, deren Name **und** Adresse in der Datei
  fehlen, hinter einer Kommentarzeile mit Quelle und Datum; vorhandene Zeilen
  (Zugangsdaten, Optionen) bleiben unberührt. Namenlose Einträge (nur
  erreichbar, nicht in der Datenbank des Quellknotens) werden übergangen.
* Verbindungsdaten: `DnConnectReq` hat `dcr_Flags` (früher `dcr_Pad`); mit
  `DCRF_DATA` liest `dnetd` die angehängten Felder `dcr_Data`/`dcr_DataLen`
  (≤ 16 Byte). `dnnodes` schickt so die NICE-Version 4.0.0 (RSX erwartet sie,
  VMS antwortet auch ohne). Ältere Aufrufer (Feld = 0) bleiben kompatibel.

### CTERM ausgehend (Stand 24.10.2026)
* Schritt 1 des Terminal-Zusatzpakets: portabler CTERM-Client
  `src/cterm/cterm.c` (Objekt 42), Test unter Linux mit
  `dntool [-v|-vv] login NODE` (Rohmodus-Terminal, Ctrl-] beendet).
  Vorlage: dnprogs `dnlogin` (cterm.c, tty.c, found.c).
* Zwei Schichten: **Foundation** (BIND vom Host → BIND-ACCEPT, UNBIND,
  ENTER-MODE → NO-MODE, COMMON-DATA mit beliebig vielen CTERM-Nachrichten,
  je 2 Byte Länge LE) und **CTERM** (INITIATE-Austausch, START-READ, WRITE,
  READ-/CHARACTERISTICS, OOB, UNREAD, CHECK-INPUT).
* CTERM ist kein Zeichenstrom: VMS schickt START-READ mit Prompt,
  Terminatormenge, Echo-/Editier-/Großschreib-Flags, Maximallänge und
  Timeout; der Client editiert die Zeile lokal und schickt READ-DATA erst
  beim Terminator (Daten ab Offset 8, Terminatorposition in Byte 6–7).
  Cursortasten ohne lokale Bedeutung (z. B. Pfeil hoch) gehen als
  „valid escape“ an den Host, DCL holt dann selbst den Befehl zurück.
  ^C/^Y/^O kommen als OOB (Zeichenattribute per CHARACTERISTICS 0x202).
* Abweichungen von dnlogin: WRITE-Daten beginnen bei Offset **5** (Typ,
  Flags 2, Präfix, Postfix — bestätigt durch dnprogs `ctermd.c`), dnlogin
  gab das Postfix-Byte mit aus; Terminaltyp in CHARACTERISTICS ist ein
  Zähl-String (Länge im ersten Byte, nicht `buf[0]`); NUL-Füllzeichen von
  VMS werden nicht ausgegeben.
* Getestet 24.10.: HOWIE (OpenVMS 7.2) komplette Sitzung (Login ohne
  Passwort-Echo, DCL, Recall, ^C/^Y, LOGOUT → UNBIND), EUGENE (VMS 5.5-2)
  bis `Username:` — gleiche Aushandlung. Mitschnitte
  `captures/2026-10-24-cterm-*.pcap`. Das Modul übersetzt für den 68000
  (≈ 6 KB) und wird so in `dnterm.device` übernommen.
* **Vollbild (24.10.):** VMS führt ein CTERM-Terminal ohne `ANSI_CRT`,
  `DEC_CRT`, `Advanced_video` — EVE/TPU bricht ab („SYS$INPUT must be
  supported CRT“). Weder Terminaltyp (logisch 0x103: VT100/VT102/VT200
  setzen nur `Device_Type`, VT220 wird „Unknown“) noch Attribute (0x102:
  Bit 1 = Scope) ändern das. Erst `SET TERMINAL/INQUIRE` (Antwort des
  Terminals auf `ESC [ c` läuft als Escape-Sequenz durch CTERM) oder
  `/DEVICE=VT100` setzt die Fähigkeiten — wie bei LAT/Telnet, gehört in
  LOGIN.COM bzw. die Doku. Danach getestet: EVE schreibgeschützt (Layout,
  Statuszeile, Cursortasten), MONITOR SYSTEM (Rahmen in DEC-Grafikzeichen,
  laufende Aktualisierung), beide mit ^Z verlassen. `dntool -t TYP` wählt
  den gemeldeten Typ (Standard VT200).

### dnterm.device (Stand 24.10.2026)
* Schritt 2: `src/term/dnterm.c`, ein Exec-Device mit der Schnittstelle
  von serial.device (CMD_READ/WRITE/CLEAR/RESET/FLUSH, SDCMD_QUERY/
  SETPARAMS/BREAK, START/STOP). Terminalprogramme (getestet: Term 4.8)
  wählen es statt serial.device. Zusatzpaket `dist/DECnet-Term.lha`
  (`tools/mkdist.sh`, Installer `installer/term/`), kopiert nur nach DEVS:.
* **Modem-Emulation** statt Konfigurationsdatei: `ATD KNOTEN|area.node`
  baut die CTERM-Verbindung auf (Name über `DnNodeAddr`, ATDT/ATDP-Präfix
  wird nur abgezogen, wenn der volle Name unbekannt ist — sonst würde aus
  „TOPS20“ „OPS20“), `CONNECT`/`NO CARRIER`, Carrier-Bit 5 in io_Status
  (low aktiv), `+++` mit 1 s Pause davor/danach, ATO, ATH, ATZ, E/Q/V,
  ATI; übrige AT-Befehle werden mit OK quittiert (Init-Strings). Taste
  während des Wählens bricht ab (Verbindung wird bei Ankunft geschlossen).
  Ablehnungsgründe kommen als Textzeile vor dem Ergebnis („Node has no
  CTERM server“ bei Grund 4). So funktionieren die Telefonbücher.
* **Ein Prozess pro Unit** (0–3, unabhängige Sitzungen; VMS: RTA1:/RTA2:
  gleichzeitig getestet). BeginIO stellt alles in den Unit-Port; Lesen
  wartet in einer Liste, die Unit füllt sie aus einem 8-KB-Ringpuffer.
  AbortIO findet Requests über private io_Flags-Bits (im Port / in der
  Liste) unter Forbid. Der letzte CloseDevice schickt eine Quit-Nachricht
  und wartet; die Unit legt auf und antwortet unter Forbid.
* **Kein decnet.library-Aufruf für Daten:** die Library blockiert, ein
  Terminal muss gleichzeitig lesen und schreiben. Das Device spricht
  dnetds privates Protokoll (`dnmsg.h`) direkt: ein DNC_READ bleibt bei
  dnetd liegen (Antwort auf netport), DNC_WRITE/DNC_CLOSE warten auf einem
  zweiten Port. dnserver erlaubt Read und Write gleichzeitig pro Link. Ist
  der Ringpuffer fast voll, wird kein neues Read gestellt (Flusskontrolle
  zum Host). decnet.library nur für DnNodeAddr.
* Bauen ohne Startup-Code wie dn-handler: `norun.c` muss die erste Datei
  sein (Inline-Stubs aus den NDK-Headern landen sonst vor DevNoRun),
  `miniprintf.c` für vsnprintf; libnix' `__mulsi3`/`__udivsi3` springen
  über `UtilityBase` — DevInit öffnet utility.library. ≈ 15 KB.
* **Fund beim Test mit Term:** Term antwortet im VT220-Modus auf `ESC [ c`
  mit 8-Bit-CSI (`9B ? 62;… c`). Der CTERM-Kern kannte nur ESC als
  Escape-Anfang, die Antwort landete als Text in der Zeile und
  `SET TERMINAL/INQUIRE` scheiterte („unknown terminal type“). Jetzt werden
  0x9B/0x8F als `ESC [`/`ESC O` gesammelt und 7-bittig an den Host
  gegeben. Danach mit Term: INQUIRE → VT200_Series/ANSI_CRT, EVE, MONITOR.
* Getestet 24.10. mit `dnttest` (Skript-Tester im Scratchpad: send/wait/
  carrier/abort/close) auf A3000/OS 3.1 (uaenet) und A2000/68000/OS 2.04
  (Installer-Skript + Test). Term 4.8 zeigt MONITOR-Rahmen als Buchstaben
  (q/x/l), solange sein GFX-Font nicht eingerichtet ist — Term-Sache.
  Mitschnitte `captures/2026-10-24-dnterm-*.pcap`.

## Verzeichnisaufbau

```
src/core/      routing.c nsp.c session.c timer.c buf.c   (portabel)
src/amiga/     dnetd.c sana2.c library/ (decnet.library)
src/linux/     dnhost.c (AF_PACKET + pcap-Replay)
src/apps/      dncopy dndir fotofix
src/cterm/     CTERM-Client (portabel, dntool login / dnterm.device)
src/term/      dnterm.device (serial.device-Ersatz mit Modem-Emulation)
src/handler/   DN-Handler
tests/         Kern-Tests, pcap-Fälle
tools/         Amiberry-IPC, Typ-Helfer (vorhanden)
s2dump/ dnhello/   Testprogramme (vorhanden)
```

## Lizenz und Quellen

GPL (entschieden). Linux-DECnet und dnprogs dienen als Referenz für Logik
und Sonderfälle; der Kern wird kompakt neu geschrieben, weil der
Linux-Code eng an `sk_buff`/Sockets hängt. PyDECnet als gut lesbare
Referenz; DEC-Specs (Routing 2.0, NSP 4.0, DAP 5.6) als Grundlage.

## Entscheidungen (21.10.2026)

1. **API-Form**: `decnet.library` (Shared Library, Messages an `dnetd`).
2. **Mindestziel**: AmigaOS 2.04 (V37), 68000-kompatibel (`-m68000`).
3. **Testadresse** für den Kern unter Linux auf dem Pi: **23.203**
   (MAC `AA-00-04-00-CB-5C`), bei RetroStar als temporäre Testmaschine
   markiert. Amiga = AMI 23.202.
