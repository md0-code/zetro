# Note

Folosirea calculatoarelor. Tastele și ecranul de configurare sunt în [README](../README.ro.md); setările, linia de comandă și limitările, în [notele în engleză](NOTES.md).

## Folosirea ZetRo

### Casete, dischete și ROM-uri

**Casetele** sunt fișiere TAP, TZX, PZX, CSW sau WAV; TAP, TZX și PZX se pot și scrie. Se pune una în poziția casetei (pagina
Media, sau se trage peste fereastră; cu Încărcare automată pornită la casetă, adică implicit, calculatorul repornește și o
încarcă singur). Pentru a salva un program pe casetă se apasă Shift+F5 înainte ca SAVE să înceapă să scrie și din nou când s-a
terminat; când poziția este goală se începe un `media/tape-NNNN.tzx` nou. Cu Încărcare rapidă pornită (comutatorul casetei de
pe pagina Media, marcat `*`, pornit implicit) o casetă se încarcă mai repede decât o încărca calculatorul original: la viteză
maximă cât timp rulează și, la compatibilele Spectrum, un bloc dintr-odată în loc de secunde, SAVE direct pe casetă și
așteptările după bandă sărite de încărcătoarele proprii. Oprită, caseta se încarcă la fel ca pe calculatorul însuși.

**Dischetele** sunt imagini DSK sau EDSK (FM și MFM, cu sectoarele slabe și octeții de stare păstrați) sau imagini brute după
mărime. Se numesc în secțiunea `media` din `zetro.yaml`, se aleg pe pagina Media sau se trag peste fereastră. Ce scrie
calculatorul se întoarce în fișierul imaginii când unitatea se oprește, deci se păstrează o copie a dischetei care contează;
protecția la scriere a poziției o oprește. Unitățile se aud ca cele reale (motor, clicuri de pas), sintetizate din ce fac
unitățile; `drive_sound` le oprește.

**ROM-urile** sunt în `roms/` în folderul de lucru, lângă configurație și lângă program. ZetRo le recunoaște după SHA-256 al
conținutului, oricum s-ar numi fișierele, deci și copiile redenumite merg; `zetro --list-roms` arată ce a găsit și ce lipsește.
Un calculator ale cărui ROM-uri lipsesc arată un ecran de probă care spune asta; ecranul de probă însuși (`zetro --model test`)
nu are nevoie de niciunul.

### Încărcare rapidă și funcțiile Spectrum

La calculatoarele compatibile Spectrum (JET, familia HC în afară de HC-88, CIP, TIM-S și CoBra):

- **Instantanee**: se trage un fișier `.sna`, `.z80` sau `.szx` peste fereastră, sau se folosește pagina Salvări. Un instantaneu
  păstrează procesorul, memoria și chenarul; ROM-ul este întotdeauna al calculatorului.
- **Poke-uri**: un fișier `.pok` este o listă de poke-uri, fiecare câteva locații de memorie și valorile de pus în ele. Se trage
  unul peste fereastră sau se încarcă pe pagina Poke-uri; fișierul `NUME.pok` al unei casete, lângă ea sau într-un folder `POKES`,
  vine singur. Enter (sau `+` și `-`) pornește și oprește un poke; unul care cere o valoare (un număr de vieți) o întreabă. Un
  poke pornit își scrie valorile din nou la fiecare cadru; oprirea lui pune înapoi ce era.
- **Temporizare Spectrum**: opțiunea dă calculatorului memoria cu acces contestat, magistrala flotantă și temporizarea cadrului
  Spectrum-ului Sinclair, pentru programele sincronizate după el; clonele nu le au.
- **Placa AY și manetele**: o placă de sunet AY (mono, stereo ABC sau ACB) și manete de tipurile Kempston, Sinclair I, Sinclair
  II, Cursor și Fuller la fiecare model, marcate `*` unde calculatorul original nu avea.

**Manetele** se comandă de la tastatură (tastele cursor, Alt stâng pentru foc și Ctrl stâng pentru al doilea buton, pornite și
oprite cu F11; `controllers.keys` le schimbă) sau cu un controler de joc (crucea sau joystick-ul stâng, A și B).

### Salvări și întoarcere în timp

F2 salvează tot ce face calculatorul în poziția curentă, iar F3 pune totul la loc, astfel încât un joc poate fi reluat exact de
unde a rămas. Fiecare model are zece poziții, păstrate ca fișiere într-un folder `states` de lângă `zetro.yaml`
(`states/hc91/slot-1.zrs`); `ui.state_slot` spune care este cea curentă. O salvare ține minte casetele și dischetele care erau în
calculator, după nume și conținut; dacă un fișier lipsește sau s-a schimbat, linia de mesaje o spune. Cât timp F4 este apăsată,
calculatorul rulează înapoi prin ultimele minute, de aproximativ zece ori mai repede, iar când F4 este lăsată calculatorul își
continuă lucrul de acolo. Se întoarce numai calculatorul, nu și fișierele folosite: ce a scris un program pe o dischetă rămâne
scris.

### Tastatura de pe ecran

F8 (sau butonul Select/Back al unui controler de joc) deschide tastatura proprie a calculatorului peste partea de jos a imaginii,
desenată în cadru ca restul imaginii, cu cuvintele-cheie și cu tastele native ale calculatorului. Un clic cu mouse-ul sau cu
degetul apasă o tastă cât timp butonul este ținut; tastele de comutare rămân apăsate până la următoarea tastă tastată, deci
numai cu mouse-ul se tastează caractere cu comutare și cuvinte-cheie. Cu un controler de joc, crucea direcțională trece de la o
tastă la alta, A apasă tasta aleasă și B închide tastatura. Calculatorul își continuă lucrul, iar tastele tastaturii gazdă
funcționează ca de obicei.

### Interfețe

Calculatoarele compatibile Spectrum pot primi o interfață adăugată (neoriginală pentru oricare dintre ele), opțiunea Interfață
pe pagina Calculator (`machine.options.interface`), niciuna într-o configurație nouă: DivMMC (un card SD) și DivIDE (două discuri
IDE), care adaugă calculatorului un card sau un disc și esxDOS, și cartușul ZX Dandanator Mini. Le primesc JET, HC-85/90/91, HC-91
Plus, HC-2000, CIP-01/02/03 și CIP-04; un HC cu Interfața 1 pornită nu poate avea una.

**DivMMC și DivIDE.** esxDOS 0.8.9 este în `roms/`. Cardul (o imagine SD, `.img`) sau discurile (`.img`, `.vhd` sau `.hdf`) se pun
în pozițiile „DivMMC card / IDE disk 1” și „IDE disk 2” de pe pagina Media (`media.card.image`, `media.card2.image`), iar
interfața se alege cu Interfață; calculatorul repornește și esxDOS arată liniile de pornire. Un card are nevoie de o partiție
FAT16 sau FAT32 cu folderele SYS și BIN ale distribuției esxDOS (esxdos.org); „New blank card” de pe pagina Media face o
imagine goală (64 Mocteți, 256 Mocteți sau 1 Gocteți) de umplut de pe un calculator, iar într-o copie a sursei `python
tools/make_card.py card.img` face un card cu esxDOS deja pus. esxDOS încarcă și salvează de pe card (`LOAD *"nume"`, `SAVE
*"nume"`), are comenzi cu punct (`.ls` listează cardul) și un meniu pe tasta NMI (F10).

**Dandanator.** Se pune un set de ROM-uri de 512 Kocteți (`.rom` sau `.bin`) în poziția „Dandanator ROM set”
(`media.romset.image`) și se alege Dandanator la Interfață: calculatorul pornește în meniul cartușului, un program pornește prin
tasta lui, iar F10 este butonul cartușului. Cu „Dandanator programming” pornit, calculatorul pornește în BASIC-ul său, iar un
program de scriere încărcat de pe casetă scrie fișierul setului. Un fișier gol pus în poziție devine un set gol.

### Imprimante și porturi seriale

Liniile de imprimantă și cele seriale ale unui calculator sunt conectate la ceva de pe calculatorul pe care rulează ZetRo; până
nu se spune la ce, nu sunt conectate la nimic. Pagina Porturi arată porturile calculatorului care rulează, fiecare cu conexiunea
ei; se scriu în secțiunea `ports` din `zetro.yaml` și se aplică imediat:

```
ports:
  printer: file:printer.out       # ce se tipărește se adaugă la fișier
  serial: tcp-listen:2323         # așteaptă un program (un terminal, telnet 127.0.0.1 2323) să se conecteze
```

Un capăt este `none`, `loopback` (un port serial legat la el însuși), un port serial al calculatorului (`COM3`,
`/dev/ttyUSB0`), `pipe:NUME`, `tcp:GAZDA:PORT`, `tcp-listen:[GAZDA:]PORT` sau `file:FIȘIER`; [notele în engleză](NOTES.md#ports)
(în engleză) are amănuntele.

### Pornirea fiecărui calculator

| Calculator | La pornire |
|---|---|
| JET, HC-85/90/91, HC-2000, TIM-S, CoBra (în BASIC) | BASIC-ul calculatorului; `LOAD ""` încarcă caseta |
| HC cu Interfața 1 | La HC-85, `FORMAT "d";1` formatează în unitatea A o imagine de dischetă goală; `CAT 1`, `SAVE *"d";1;"nume"`, `LOAD *"d";1;"nume"`. La HC-90 și HC-91 se alege CP/M la Pornire (Interfața 1 pornește odată cu ea) și se pune în unitatea A o dischetă de sistem CP/M, sau se tastează `PRINT USR 14446` la HC-91; `DIR` listează fișierele |
| HC-91 Plus | Un meniu în stilul 128K; cu Interfața 1 pornită, CP/M pornește ca la HC-91 |
| HC-2000 | Pornirea CP/M pornește de pe discheta de sistem din unitatea A |
| HC-88 | Pornește CP/M de pe discheta de sistem din unitatea A; `BASIC` pornește interpretorul de pe casetă |
| CIP-03, CIP-02 | Prompt-ul `Basic-S`: se apasă Enter, apoi se tastează ca pe un Spectrum 48K |
| CIP-01 | Un copiator cu meniul L (lansare), I (încărcare), S (salvare), V (verificare): se apasă L, Enter, Enter și se pornește caseta cu BASIC; apoi `LOAD ""`. Cu Încărcare automată pornită, la o casetă pusă cât copiatorul este pe ecran se tastează comenzile copiatorului, iar după ce BASIC-ul rulează se tastează `LOAD ""` fără repornire |
| CIP-04 | Meniul +3: Enter încarcă de pe casetă sau de pe dischetă; poziția `a` primește o imagine DSK |
| TIM-S | Pornește din ROM, îl copiază în RAM și rulează de acolo; F10 este tasta NMI |
| TIM-S Plus | Un meniu cu Loader, +3 BASIC, Calculator și 48 BASIC; se pune în unitatea A o imagine de dischetă goală și se folosesc `SAVE "a:nume"`, `LOAD "a:nume"` și `CAT` |
| CoBra | Meniul programului de pornire: B pentru BASIC, C pentru încărcătorul de casetă, D pentru CP/M de pe discheta din unitatea A; opțiunea Pornire apasă tasta singură |
| CUB Z | Monitorul său (`cubZ V1.4`, prompt `.`) pornește CP/M de pe discheta din unitatea A; cu o dischetă pusă mai târziu, reinițializare sau `GF800` și Enter |
| aMIC | Un monitor cu prompt `*`: `G0800` și Enter pornesc BASIC-ul; `LOA` încarcă de pe casetă, `SAV` salvează, `RUN` rulează |
| PRAE-M | Enter pentru consola locală, Enter peste pagina de întâmpinare și Enter la HIGHEST MEMORY?; apoi `ALOAD "NUME"` încarcă un program de pe casetă, `LOAD "NUME" &6000` o imagine de memorie și `ASAVE "NUME"` salvează. Cu Încărcare automată pornită, la o casetă pusă se tastează comanda cu numele citit din antetul ei |

### Comenzi din script

`zetro --hidden --script FIȘIER` rulează un fișier de comenzi fără a deschide o fereastră, iar `zetro --control stdin` (sau
`tcp:PORT`) primește aceleași comenzi câte una pe linie și răspunde la fiecare cu o linie JSON: tastare, taste, suporturi, ecrane
salvate, textul de pe ecran, salvări, instantanee, poke-uri, întoarcere în timp, viteză, memorie. O rulare ascunsă poate
comanda și ecranul de configurare (`--setup-page`, `--hostkey`) și poate salva ce arată fereastra cu `--screenshot`. `zetro
--help` listează toate comutatoarele și comenzile, iar [notele în engleză](NOTES.md#configuration) le descrie.
