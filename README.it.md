# Flying Layers per OBS Studio

[English](README.md)

Transizione di scena per OBS Studio che fa *volare* i layer da una scena all'altra:

- **i layer presenti in entrambe le scene** si spostano e si ridimensionano dalla posizione della prima scena a quella della seconda; posizione, scala, bounding box, rotazione e crop sono tutti animati;
- **i layer che escono** (presenti solo nella prima scena) spariscono con dissolvenza, zoom, scorrimento verso un bordo, wipe o taglio;
- **i layer che entrano** (presenti solo nella seconda scena) appaiono con gli stessi effetti;
- l'audio fa una dissolvenza incrociata, come la transizione Dissolvenza.

L'animazione segue fotogramma per fotogramma l'orologio video di OBS, quindi è fluida a qualsiasi frequenza. Ogni sorgente viene disegnata una sola volta per fotogramma e le scene non vengono mai modificate.

Richiede OBS Studio 31.1 o successivo, su macOS (Apple Silicon e Intel) o Windows (64 bit).

## Download

Scarica l'ultima versione da [Releases](https://github.com/YearNorth/flying-layers/releases/latest):

| Sistema | File |
| --- | --- |
| Windows | `flying-layers-<versione>-windows-x64.zip` |
| macOS | `flying-layers-<versione>-macos-universal.pkg` (installer) oppure `.tar.xz` (solo plugin) |

## Installazione

**Windows.** Chiudi OBS ed estrai lo zip in `C:\ProgramData\obs-studio\plugins\`, in modo che esista il file `C:\ProgramData\obs-studio\plugins\flying-layers\bin\64bit\flying-layers.dll`.

**macOS.** Chiudi OBS, apri il `.pkg` e segui l'installer. In alternativa estrai il `.tar.xz` e copia `flying-layers.plugin` in `~/Library/Application Support/obs-studio/plugins/`.

Il pacchetto macOS non è notarizzato. Se macOS lo blocca, apri **Impostazioni di Sistema → Privacy e sicurezza** e scegli **Apri comunque**. Per la versione solo plugin, esegui `xattr -cr` su `flying-layers.plugin`.

## Uso

Nel pannello **Transizioni di scena** clicca **+** e scegli **Flying Layers**. Imposta la durata come per qualsiasi altra transizione e apri le **Proprietà** (icona a ingranaggio) per le opzioni:

| Gruppo | Opzioni |
| --- | --- |
| Curva del movimento | Lineare, ease in, ease out, ease in-out, morbida, back (supera e si assesta), back in-out, elastica, oppure una curva Bézier personalizzata (come `cubic-bezier` in CSS) |
| Tempi | **Tutto insieme** oppure **in sequenza** (prima escono i layer, poi volano, poi entrano). Parte della durata per i layer in uscita e in entrata. **Cascata**: i layer partono uno dopo l'altro, nell'ordine dei layer o secondo la posizione sullo schermo |
| Layer in entrata e in uscita | Dissolvenza, zoom, scorrimento, wipe o taglio, scelti separatamente per entrata e uscita. Bordo per scorrimento e wipe (più vicino, sinistra, destra, alto, basso). Dissolvenza opzionale durante zoom e scorrimento |
| Motion blur | Sfoca i layer lungo il movimento, come l'otturatore di una telecamera. L'angolo dell'otturatore regola l'intensità (180° è l'aspetto cinematografico, 360° il fotogramma intero). Con la T-bar ferma l'immagine è nitida |

Funziona ovunque funzionino le transizioni di OBS: cambio scena diretto, Studio Mode con la T-bar, hotkey, Stream Deck e obs-websocket.

Come vengono abbinati i layer:

- due layer sono lo stesso layer se mostrano la stessa sorgente. Se una sorgente compare più volte in una scena, la prima copia si abbina alla prima, la seconda alla seconda, e così via;
- durante il volo i layer seguono l'ordine della scena di destinazione. I layer in uscita restano subito sopra il layer che avevano sotto;
- gruppi e scene annidate volano come un unico blocco;
- i layer nascosti non partecipano.

## Compilare dai sorgenti

A ogni push GitHub Actions compila il plugin per macOS e Windows. Il push di un tag di versione (per esempio `1.0.0`) crea una release in bozza con i pacchetti allegati.

Per compilare in locale serve l'ambiente di [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate): Xcode 16 o successivo su macOS, Visual Studio 2022 su Windows. Le dipendenze vengono scaricate automaticamente.

```bash
cmake --preset macos
cmake --build --preset macos
```

```bash
cmake --preset windows-x64
cmake --build --preset windows-x64
```

## Licenza

GPL-2.0 o successiva, vedi [LICENSE](LICENSE). La geometria degli elementi di scena in `src/layout.cpp` e il vertex shader in `data/flying-layers.effect` riprendono codice di OBS Studio, che ha la stessa licenza.
