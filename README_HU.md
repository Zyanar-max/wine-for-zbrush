# Wine for ZBrush

Javítások ahhoz, hogy a **ZBrush** Linuxon, **Bottles**-ben rendesen működjön. Telepítsd a ZBrusht egy bottle-be, futtass le egy szkriptet, és kész.

> **Ezt a saját gépemre készítettem, és úgy osztom meg, ahogy van. Támogatást nem adok hozzá, kérlek, ne keress meg vele.** Ha nálad nem működik, a `sh install.sh --uninstall` mindent visszaállít. Csak saját felelősségre használd.
>
> Vibe coding, [Claude](https://claude.com/claude-code)-dal (Claude Code) készült: a Wine-hibák felderítése és javítása, a fájlablak és a telepítő is Claude-dal közös munkában született.

| # | Probléma | Megoldás |
|---|----------|----------|
| 1 | Elszáll a tollnyomás, amint hozzányúlsz az egérhez | Wine-javítás (winex11) |
| 2 | Elszáll a tollnyomás egy ablak (pl. Import) után | Wine-javítás (wintab32) |
| 3 | Tearing a vászon tetején forgatáskor | Wine-javítás (winex11) |
| 4 | Csúnya, windowsos Open/Import/Save/Export ablak | A Linuxod saját fájlválasztója (GNOME/KDE) jön fel |
| 5 | A Decimation Master 90%-nál elakad vagy összeomlik | Ellenőrzi a Microsoft-féle `vcomp140`-et és a beállítást |
| 6 | Betöltéskor 5 mp után: *„ZBrush is not responding”* | Opcionális: a GNOME 20 mp-et vár |
| 7 | A ZBrush-fájlok (.ztl, .zpr, .zbp, .zmt, .zsc…) üres lapként látszanak, és nem nyílnak meg | A ZBrush saját fájlikonjai, és dupla kattintásra megnyílnak a ZBrushban |

## Ami kell hozzá

- [Bottles](https://usebottles.com) (Flatpak vagy natív)
- A Bottlesben a **soda-11.0-10** runner (*Beállítások → Runners*)
- Egy bottle-be telepített ZBrush (kipróbálva: ZBrush 2023.2)
- X11 asztal vagy XWayland. Kipróbálva: Rocky Linux 9, GNOME, X11.

## Telepítés

1. Zárd be a ZBrusht és a Bottlest.
2. Futtasd:
   ```sh
   sh install.sh
   ```
   Magától megkeresi azt a bottle-t, amelyikben a `ZBrush.exe` van. Ha több ilyen van: `sh install.sh --bottle NÉV`.
3. Indítsd a ZBrusht a Bottlesből, ahogy eddig.

**Utána ne válts runnert a bottle-nél a Bottlesben.** Ha mégis, az 1–3. javítás elvész, ilyenkor csak futtasd újra a szkriptet.

**Bottles-frissítés után** a Bottles néha visszaállítja a bottle-t, és visszajön a régi Wine fájlablak. Ilyenkor csak futtasd újra a szkriptet (`sh install.sh --bottle NÉV --yes`). Akárhányszor lefuttathatod.

### Egyszerre csak egy ZBrush

Ha elindítasz egy második ZBrusht, miközben egy már fut, a Bottles lefagy. A Bottles (Flatpak) minden indítást külön dobozban futtat, és a kettő összeakad. A kezelő ezt megakadályozza: a `~/.local/bin/wine-for-zbrush-open` fájl nélkül elindítja a ZBrusht, de ha a ZBrush már fut vagy éppen indul, csak egy értesítést kapsz.

Hogy a dokkról vagy az alkalmazásmenüből is így induljon, a ZBrush menübejegyzésében (`.desktop` fájl, pl. a `~/.local/share/applications` mappában) ezt állítsd be:

```
Exec=/home/FELHASZNÁLÓ/.local/bin/wine-for-zbrush-open
```

Ha a Bottles ablakában a ▶ gombbal indítod a ZBrusht, az nincs védve.

A kezelő értesítései sürgősek, így a GNOME a teljes képernyős ZBrush fölött is megmutatja őket. Addig maradnak kint, amíg be nem zárod őket.

## Ezeket a ZBrushban magadnak kell beállítani (telepítés után)

Ezeket a szkript nem állítja át, mert ZBrush-beállítások, és mindenki kicsit máshogy szereti.

### Tollnyomás: a Use Tablet legyen BEkapcsolva

A **Preferences → Tablet → Use Tablet** legyen bekapcsolva. Windows alatt egyes táblameghajtóknál (pl. Xencelabs) ki kell kapcsolni, ezért ha a ZBrush-configodat Windowsról hozod át, kikapcsolva jöhet. Ilyenkor a toll úgy rajzol, mint az egér, nyomás nélkül, pedig a Wine átadja a nyomást.

### Felületméret (HiDPI / 4K)

A ZBrush saját beállítását használd. **A Wine DPI-/méretezési beállítását ne** (`LogPixels` vagy a Bottles DPI-opciója), mert attól erős képhibák jelennek meg a ZBrushban.

1. **Preferences → Interface → UI → Button Size**: húzd feljebb. 4K-s kijelzőn a 70–75 körülbelül 150%.
2. Mentsd el: **Ctrl+Shift+I** (Store UI), aztán indítsd újra a ZBrusht.

### Éles vászon

Alapból a dokumentum kisebb lehet a vászonnál. Ilyenkor a ZBrush felnagyítja, ezért homályos a modell, forgatáskor pedig levágódik a széleken.

1. **Document → a Pro legyen kikapcsolva.** Ha be van kapcsolva, átméretezéskor megtartja a régi arányt.
2. A **Width / Height** legyen akkora, mint a vászon területe (4K-n pl. 3240 × 1880), aztán **Resize**. Ha letörlődik a vászon, húzd ki újra a modellt, és nyomj **T**-t.
3. Nyomj **0**-t (Actual), így 1:1-ben látszik.
4. **Document → Save As Startup Doc**, hogy legközelebb is így induljon.

Nagyobb dokumentumnál több pixelt kell újrarajzolni, ezért a forgatás kicsit lassulhat. Ha túl lassú, válassz kisebb méretet.

## Eltávolítás

```sh
sh install.sh --uninstall
```

Visszaállítja a régi runnert, az eredeti `wintab32.dll`-t és a Wine fájlablakát. A DLL-beállítások és a GNOME-várakozás maradnak.

## Mit változtat

- Létrehozza a **soda-11.0-10-zbrush** runnert. Ez a soda-11.0-10 másolata (hard linkekkel, így nem foglal több helyet), 3 apró bináris javítással. Az eredeti runnerhez nem nyúl. Javítás előtt ellenőrzi az eredeti fájlok ellenőrzőösszegét és a javítandó bájtokat. Ha bármi eltér, semmit nem módosít.
- A bottle-t erre a runnerre állítja (`bottle.yml`, mentés: `bottle.yml.bak-wine-for-zbrush`).
- A javított `wintab32.dll`-t bemásolja a bottle `system32` mappájába (az eredeti: `wintab32.dll.orig`).
- A `zbfiledlg.dll`-t és a `zbfiledlg.py`-t a `system32`-be teszi, és beállítja őket a Windows fájlablakai (*FileOpenDialog / FileSaveDialog*) helyére (`system.reg`, mentés: `system.reg.bak-wine-for-zbrush`).
- A `vcomp140`/`120`/`110`/`100` beállítását `native,builtin`-re teszi (`user.reg`).
- Felveszi a ZBrush fájltípusait (.ztl, .zpr, valamint a .zbp, .zmt, .zsc és a többi, ugyanazok, amelyeket a ZBrush Windowson regisztrál) a ZBrush saját ikonjaival (`~/.local/share/icons/hicolor`, és ha az ikontémád a `~/.local/share/icons` alatt van, pl. a Papirus, akkor abba is). Létrehoz egy rejtett „ZBrush” kezelőt is (`wine-for-zbrush-open.desktop` + `~/.local/bin/wine-for-zbrush-open`), ami dupla kattintásra megnyitja a fájlt a bottle-ben. Ha a ZBrush már fut, a fájl abba töltődik be, ahogy Windowson. A Bottles (Flatpak) minden indítást külön dobozban, saját Wine-szerverrel futtat, ezért egy második ZBrush nem látja az elsőt, és összeakadnak. Ezért a kezelő a futó ZBrush dobozában indítja a másodikat (`flatpak enter`), az átadja a fájlt, majd kilép. A fájlnak olyan mappában kell lennie, amit a bottle lát. Ha nem ilyenben van, a ZBrush *„Unable to open”* hibája helyett értesítést kapsz (*„ZBrush can't see this file”*). Hogy a Bottles lásson egy mappát, használd a Flatsealt, vagy pl. `flatpak override --user --filesystem=xdg-desktop com.usebottles.bottles` (csak az Asztal mappa).

A hibák részletes leírása angolul a `README.md`-ben van.

## Tesztgép

**Szoftver**

| | Verzió |
|---|---|
| ZBrush | 2023.2 (örökös licenc) |
| Bottles | 67.5 (Flatpak, GNOME Platform 50 runtime) |
| Wine runner | soda-11.0-10 (Wine 11.0) → javított másolat: `soda-11.0-10-zbrush` |
| Bottle | Application környezet, Windows 10, DXVK 3.1.1, VKD3D-Proton 3.0.1, sync: fsync |
| Rendszer | Rocky Linux 9.8, kernel 5.14.0-687 |
| Asztal | GNOME 40.10, **X11** (Xorg 1.20.11), xdg-desktop-portal-gnome 41.2 |
| Táblameghajtó | xorg-x11-drv-libinput 1.0.1 (libinput 1.19.3) |
| Videókártya-meghajtó | NVIDIA 580.178.04 (open kernel module) |

**Hardver**

| | |
|---|---|
| Processzor | AMD Ryzen 9 5900X (12 mag / 24 szál) |
| Memória | 128 GB |
| Videókártya | NVIDIA GeForce RTX 3090 |
| Kijelző | 2 × 4K (3840 × 2160), 60 Hz |
| Tábla | Xencelabs Pen Tablet Medium + Quick Keys (vezeték nélküli dongle) |

**Wacommal** is működnie kell, mert a hibák a Wine általános táblakezelésében vannak, de nincs kipróbálva. A Wayland (XWayland), a KDE és más disztribúciók sincsenek kipróbálva.

## Ismert korlátok

- A megnyitó ablak a *Legutóbbiak* nézetben indul, nem az utoljára használt mappában (a régebbi GNOME portál nem veszi figyelembe a kezdőmappát).
- A javítások csak a **soda-11.0-10** runnerrel működnek. Más runnernél a szkript nem csinál semmit.
- Egyszerre csak egy ZBrush futhat, és ez csak akkor van védve, ha a kezelőn keresztül indítod (lásd *Egyszerre csak egy ZBrush*).
