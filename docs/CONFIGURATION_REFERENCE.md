# Panduan Referensi Konfigurasi VFRS (VFRS Configuration Reference Manual)

> [!IMPORTANT]
> **Format Konfigurasi Otoritatif**: Parser konfigurasi modern VFRS (`cfg_parser.c` / `cfg_schema.c`) mengimplementasikan tata bahasa formal modern seperti yang didefinisikan secara otoritatif pada [**`confs/example_config_new.conf`**](file:///c:/Users/rizki/Programming/vfrns/vfr_switch/confs/example_config_new.conf) dan [**`confs/vfrs_config_guide.conf`**](file:///c:/Users/rizki/Programming/vfrns/vfr_switch/confs/vfrs_config_guide.conf). Format konfigurasi *legacy* (seperti sintaksis pada berkas `example_config-legacy.conf`) saat ini **sepenuhnya tidak didukung** (*completely unsupported*). Seluruh berkas konfigurasi VFRS wajib menggunakan tata bahasa modern yang didokumentasikan di bawah ini.

Dokumen ini merupakan panduan referensi teknis otoritatif untuk seluruh sintaksis, parameter, nilai default, validasi tipe data, dan relasi standar telekomunikasi pada **Virtual Frame Relay Switch (VFRS)**.

---

## Daftar Isi
1. [Konfigurasi Identitas Switch & Dial-Plan Global (`swconfig`)](#1-konfigurasi-identitas-switch--dial-plan-global-swconfig)
2. [Subsistem Logging & Rotasi Berkas (`log`)](#2-subsistem-logging--rotasi-berkas-log)
3. [Cascading Parameter Default Bertingkat (`default`)](#3-cascading-parameter-default-bertingkat-default)
4. [Definisi Antarmuka & Driver Transport (`port`)](#4-definisi-antarmuka--driver-transport-port)
5. [Permanent Virtual Circuits & Multicast (`pvc` & `pvc mcast`)](#5-permanent-virtual-circuits--multicast-pvc--pvc-mcast)
6. [Local Management Interface (`lmi`)](#6-local-management-interface-lmi)
7. [Protokol Data Link LAPF ITU-T Q.922 (`lapf`)](#7-protokol-data-link-lapf-itu-t-q922-lapf)
8. [Persinyalan & Layanan Panggilan Dinamis SVC (`svc int`, `svc addr`, `svc route`, `svc mcast`)](#8-persinyalan--layanan-panggilan-dinamis-svc-svc-int-svc-addr-svc-route-svc-mcast)
9. [Soft Permanent Virtual Circuits (`spvc`)](#9-soft-permanent-virtual-circuits-spvc)
10. [Inter-Switch Signaling & Management Protocol (`issmp`)](#10-inter-switch-signaling--management-protocol-issmp)
11. [Manajemen Kemacetan & Notifikasi CLLM (`cgst`)](#11-manajemen-kemacetan--notifikasi-cllm-cgst)
12. [Perekaman Paket Jaringan Live PCAP (`capture`)](#12-perekaman-paket-jaringan-live-pcap-capture)

---

## 1. Konfigurasi Identitas Switch & Dial-Plan Global (`swconfig`)

### 1.1 Tata Bahasa BNF
```bnf
<swconfig-stmt> ::= "swconfig" <swconfig-param>+

<swconfig-param> ::= "swid=" <string>
                   | "nspf=" ("x121" | "e164")
                   | <x121-params>
                   | <e164-params>
                   | "sgclen=" <1-4>
                   | "sgc=" <number>
                   | "siclen=" <1-4>
                   | "sic=" <number>
                   | "sublen=" <number>
                   | "subnumlen=" <number>

<x121-params> ::= ("dcc=" <3-digit-number> ["nd=" (<1-digit-number> | "none")])
                | "dnic=" <4-digit-number>
                | "pnic=" (<number> | "none")
                | "ind=" <number>

<e164-params> ::= "cc=" <1-3-digit-number>
                | "ic=" <1-4-digit-number>
                | "ndc=" <number> ["area=" <number>]
```

### 1.2 Tabel Parameter
| Parameter | Tipe Data | Nilai Default | Rentang / Format | Relasi Standar & Deskripsi Teknis |
| :--- | :--- | :--- | :--- | :--- |
| `swid` | String | `"vfrs0"` | Maks 32 karakter | Pengenal tekstual switch untuk identifikasi pada CLI dan berkas log. |
| `nspf` | Enum | `x121` | `x121`, `e164` | *Numbering Plan Selection* (ITU-T X.121 untuk PDN, ITU-T E.164 untuk PSTN/ISDN). |
| `dcc` | Numerik | `100` | 3 digit angka | *Data Country Code* per ITU-T X.121 (misal: `510` untuk Indonesia). |
| `nd` | Numerik | `0` | 1 digit ($0 \dots 9$) | *Network Digit* setelah DCC per ITU-T X.121. |
| `dnic` | Numerik | `1000` | 4 digit angka | *Data Network Identification Code* (DCC + ND). Menggantikan `dcc`+`nd` jika dikonfigurasi. |
| `pnic` | Numerik | `none` | 0 s.d. 6 digit | *Private Data Network Identification Code* per ITU-T X.121. |
| `ind` | Numerik | `none` | Dinamis | *Internal Network Digits* untuk partisi dan segmentasi perutean internal operator. |
| `sgclen` | Integer | `1` | $1 \dots 4$ digit | Panjang digit Kode Kelompok Sistem (KKS / *System Group Code*). |
| `sgc` | Numerik | `1` | $1 \dots 4$ digit | Nilai SGC. Otomatis di-*zero-padding* di depan sesuai `sgclen` (misal: `sgc=1`, `sgclen=2` $\to$ `01`). |
| `siclen` | Integer | `1` | $1 \dots 4$ digit | Panjang digit Kode Identifikasi Sistem (KIS / *System Identification Code*). |
| `sic` | Numerik | `1` | $1 \dots 4$ digit | Nilai SIC. Otomatis di-*zero-padding* di depan sesuai `siclen` (misal: `sic=1`, `siclen=2` $\to$ `01`). |
| `sublen` / `subnumlen` | Integer | Kalkulasi | $1 \dots 6$ digit | Panjang nomor terminal otomatis pada antarmuka UNI: $\text{sublen} = 14 - (\text{panjang DNIC} + \text{PNIC} + \text{IND} + \text{SGC} + \text{SIC})$. |
| `cc` | Numerik | `62` | $1 \dots 3$ digit | *Country Code* per ITU-T E.164 (misal `62` untuk Indonesia, `1` untuk Amerika Serikat). |
| `ic` | Numerik | `none` | $1 \dots 4$ digit | *International Identification Code* per ITU-T E.164. |
| `ndc` | Numerik | `none` | Dinamis | *National Destination Code* per ITU-T E.164. |
| `area` | Numerik | `none` | Dinamis | Kode area wilayah geografis lokal/regional per ITU-T E.164. |

### 1.3 Alamat Khusus Sistem (*Reserved System Address*)
Switch VFRS secara otomatis memesan satu nomor sistem internal:
$$\text{DNIC} + \text{SGC} + \text{SIC} + \underbrace{00\dots0}_{\text{sublen digit}}$$
*(Contoh: `510401010000`)*. Nomor ini dapat dipanggil dari semua port UNI dan NNI untuk keperluan loopback, diagnostik, dan koordinasi manajemen.

---

## 2. Subsistem Logging & Rotasi Berkas (`log`)

### 2.1 Tata Bahasa BNF
```bnf
<log-stmt> ::= "log" "level" "con=" <log-level> "txt=" <log-level>
             | "log" "file" <file-path>
             | "log" "rotation" "size=" <number> "files=" <number>

<log-level> ::= "trace" | "debug" | "info" | "warn" | "error"
```

### 2.2 Tabel Parameter
| Parameter | Nilai Default | Deskripsi Teknis |
| :--- | :--- | :--- |
| `con={...}` | `info` | Tingkat keparahan minimum log yang ditampilkan di konsol interaktif CLI. |
| `txt={...}` | `info` | Tingkat keparahan minimum log yang disimpan dalam berkas teks di disk. |
| `file <path>` | `vfrs.log` | Lokasi dan nama berkas teks untuk menyimpan rekaman aktivitas switch. |
| `size=<mb>` | `10` MB | Ambang batas ukuran berkas log sebelum dipotong dan dirotasi. |
| `files=<n>` | `5` berkas | Jumlah maksimum berkas arsip rotasi log yang disimpan (`vfrs.log.1`, `vfrs.log.2`, dst.). |

---

## 3. Cascading Parameter Default Bertingkat (`default`)

Pernyataan `default` digunakan untuk menetapkan nilai awal bertingkat (*scoped cascade defaults*) ke seluruh antarmuka yang didefinisikan sesudahnya.

### 3.1 Tata Bahasa BNF
```bnf
<default-stmt> ::= "default" "interface" ["dlcibit=" ("10" | "23")] ["ar=" <rate-bps>]
                 | "default" "lapf" ["k=" <number>] ["n200=" <number>] ["n201=" <number>] ["t200=" <time>] ["t203=" <time>]
                 | "default" "lmi-dce" ["n392=" <number>] ["n393=" <number>] ["t392=" <time>]
                 | "default" "lmi-dte" ["n391=" <number>] ["n392=" <number>] ["n393=" <number>] ["t391=" <time>]
                 | "default" "pvc" [<qos-params>]
                 | "default" "svc" [<qos-params>] ["fmif=" <number>] [<revchg-params>]
                 | "default" "svc" "mcast" [<qos-params>] ["fmif=" <number>] [<revchg-params>]
                 | "default" "svc" ("uni" | "nni") [<svc-timer-params>]
```

### 3.2 Tabel Parameter & Standar
| Lingkup (*Scope*) | Parameter | Nilai Default | Standar Acuan | Penjelasan Operasional |
| :--- | :--- | :--- | :--- | :--- |
| **`interface`** | `dlcibit` | `10` | ITU-T X.36 §3 | Kapasitas bit DLCI (10-bit standar maks 1023, 23-bit extended maks 8.388.607). |
| | `ar` | `64000` bps | ITU-T X.36 §8.2.1 | Laju transmisi fisik dasar antarmuka (*Physical Access Rate*). |
| **`lapf`** | `k` | `8` | ITU-T Q.922 §5.9 | Ukuran *sliding window* frame informasi LAPF ($1 \dots 127$). |
| | `n200` | `3` | ITU-T Q.922 §5.9 | Jumlah transmisi ulang maksimum sebelum link LAPF di-reset. |
| | `n201` | `260` oktet | ITU-T Q.922 §5.9 | Ukuran maksimum *information field* I-frame LAPF. |
| | `t200` | `2s` | ITU-T Q.922 §5.9 | Pewaktu transmisi ulang I-frame (*Retransmission timer*). |
| | `t203` | `30s` | ITU-T Q.922 §5.9 | Pewaktu pemantauan link idle keepalive (*Link idle timer*). |
| **`lmi-dce`** | `t392` | `15s` | ITU-T X.36 §11.6 | Pewaktu verifikasi polling sisi DCE ($5 \dots 30\text{s}$). |
| | `n392` | `3` | ITU-T X.36 §11.6 | Ambang batas kesalahan DCE ($1 \dots 10$). |
| | `n393` | `4` | ITU-T X.36 §11.6 | Jendela kejadian yang dipantau DCE ($1 \dots 10$). |
| **`lmi-dte`** | `t391` | `10s` | ITU-T X.36 §11.5 | Interval pengiriman polling status DTE ($5 \dots 30\text{s}$). |
| | `n391` | `6` | ITU-T X.36 §11.5 | Siklus permintaan status penuh (*Full Status counter*, $1 \dots 255$). |
| | `n392` | `3` | ITU-T X.36 §11.5 | Ambang batas kesalahan DTE ($1 \dots 10$). |
| | `n393` | `4` | ITU-T X.36 §11.5 | Jendela kejadian yang dipantau DTE ($1 \dots 10$). |
| **`pvc` / `svc`** | `cir` | `32000` bps | ITU-T X.36 §8.2 | *Committed Information Rate*. |
| | `bc` | `32000` bit | ITU-T X.36 §8.2 | *Committed Burst size* ($T_c = B_c / \text{CIR}$). |
| | `be` | `0` bit | ITU-T X.36 §8.2 | *Excess Burst size* (frame ditandai bit DE per §8.2). |
| | `ftp` | `8` | ITU-T X.36 §7.4 | *Frame Transfer Priority* ($0 \dots 15$). |
| | `fdp` | `4` | ITU-T X.36 §7.5 | *Frame Discard Priority* ($0 \dots 7$). |
| | `class` | `1` | ITU-T X.146 | *Service Class* ($0 \dots 3$ per Tabel 7-1 X.36). |
| | `fmif` | `1600` oktet | ITU-T X.36 §8.3.3 | *Maximum Frame Information Field Size* untuk SVC. |
| | `revchg` | `allow` | ITU-T X.36 Annex B | Kebijakan izin penerimaan panggilan beban balik (*Reverse Charging*). |
| **`svc uni/nni`** | `t303` | `4s` | ITU-T X.36 Tab 10-27 | Pewaktu pengiriman SETUP menunggu respons. |
| | `t305` | `30s` | ITU-T X.36 Tab 10-27 | Pewaktu pengiriman DISCONNECT menunggu RELEASE. |
| | `t308` | `4s` | ITU-T X.36 Tab 10-27 | Pewaktu pengiriman RELEASE menunggu RELEASE COMPLETE. |
| | `t310` | `35s` / `40s` | ITU-T X.36 Tab 10-27 | Pewaktu menunggu CONNECT setelah CALL PROCEEDING. |
| | `t301` | `180s` | ITU-T X.36 Tab 10-27 | Pewaktu Alerting menunggu jawaban CONNECT. |
| | `t316` | `120s` | ITU-T X.36 Tab 10-27 | Pewaktu pengiriman RESTART. |
| | `t317` | `10s` / `20s` | ITU-T X.36 Tab 10-27 | Pewaktu watchdog pembersihan internal penerimaan RESTART. |
| | `t322` | `4s` | ITU-T X.36 Tab 10-27 | Pewaktu pengiriman STATUS ENQUIRY Layer 3. |

---

## 4. Definisi Antarmuka & Driver Transport (`port`)

### 4.1 Tata Bahasa BNF
```bnf
<port-stmt> ::= "port" <port-name> <transport-driver> [<interface-params>]

<port-name> ::= ("uni" | "nni") <0-255> "/" <0-255>

<transport-driver> ::= "pipe-client" <pipe-name>
                     | "pipe-server" <pipe-name>
                     | "l2tpv3fr" [<local-host>] <remote-host> <base-vcid>
                     | "l2tpv3hdlc" [<local-host>] <remote-host> <vcid>
                     | "serial" <serial-device> ["baud=" <number>]
                     | "tcp" [<local-host>] <local-port> <remote-host> <remote-port>
                     | "tcp-client" <remote-host> <remote-port>
                     | "tcp-server" [<bind-ip>] <local-port>
                     | "udp" [<local-host>] <local-port> <remote-host> <remote-port>
                     | "udp-client" <remote-host> <remote-port>
                     | "udp-server" [<bind-ip>] <local-port>

<interface-params> ::= ["dlcibit=" ("10" | "23")] ["ar=" <rate-bps>]
```

### 4.2 Skala & Ketentuan Antarmuka
1. **Penamaan Antarmuka**: Menggunakan notasi ala Cisco `uni<G>/<P>` (DCE sisi jaringan) dan `nni<G>/<P>` (STE simetris). $G = 0 \dots 255$, $P = 0 \dots 255$.
2. **Kapasitas Port**: Mendukung hingga $65.536$ port per tipe antarmuka (total kapasitas $131.072$ port per instans switch).
3. **Framing DynaMIPS / GNS3**: Transport TCP dan Named Pipe menerapkan prefix panjang 4-byte *big-endian*, memungkinkan interkoneksi langsung dengan router Cisco pada GNS3 / DynaMIPS.
4. **Auto-Reconnection**: Klien TCP (`tcp-client`) menerapkan *exponential backoff reconnection* otomatis ($1\text{s} \dots 60\text{s}$) bila koneksi terputus.
5. **Auto-Derived Access Rate**: Port serial secara otomatis menurunkan nilai Access Rate ($AR$) dari kecepatan baud rate yang dikonfigurasi (maksimum hingga 8.192.000 bps).

### 4.3 Alokasi Ruang Alamat DLCI (ITU-T X.36)
```
+------------------+--------------------------------------------------------+
| Rentang DLCI     | Fungsi & Peruntukan Protokol                           |
+------------------+--------------------------------------------------------+
| 0                | Reserved untuk LMI (UI-frame) & Persinyalan SVC (I-frame)|
| 1 - 15           | Reserved for future use (Standar ITU-T)                |
| 16 - 991         | Sirkuit Data Pengguna (PVC dan alokasi dinamis SVC)    |
| 992 - 1007       | Layer 2 Management (CLLM XID frame pada DLCI 1007)     |
| 1008 - 1022      | Reserved for future use                                |
| 1023             | Reserved untuk Cisco / Gang of Four LMI                |
| 1024 - 8.388.607 | Extended User Circuits (Hanya pada mode dlcibit=23)    |
+------------------+--------------------------------------------------------+
```

---

## 5. Permanent Virtual Circuits & Multicast (`pvc` & `pvc mcast`)

### 5.1 Tata Bahasa BNF
```bnf
<pvc-stmt> ::= "pvc" <port-1> <dlci-1> <port-2> <dlci-2> [<pvc-qos-params>]

<pvc-mcast-stmt> ::= "pvc" "mcast" <group-name> [<pvc-qos-params>]
                   | "pvc" "mcast" "group" <group-name> <src-port> <src-dlci> ("oneway" | "twoway" | "nway")
                   | "pvc" "mcast" "member" <group-name> <mem-port> <mem-dlci> [<pvc-qos-params>]

<pvc-qos-params> ::= ["cir=" <rate-bps>] ["bc=" <bits>] ["be=" <bits>]
                     ["ftp=" <0-15>] ["fdp=" <0-7>] ["class=" <0-3>]
```

### 5.2 Mekanisme Traffic Policing Tiga Tingkat (ITU-T X.36 §8.2)
- **Lalu Lintas Committed (Dalam $B_c$)**: Diteruskan tanpa perubahan (*transparent passthrough*).
- **Lalu Lintas Excess (Melebihi $B_c$ hingga $B_c + B_e$)**: Diteruskan dengan bit **DE (*Discard Eligibility*) = 1**.
- **Lalu Lintas Non-Conforming (Di luar $B_c + B_e$)**: Diberi prioritas terendah dan langsung dibuang (*dropped*) saat terjadi kemacetan buffer transmisi.

### 5.3 Karakteristik Grup Multicast (FRF.7 / ITU-T X.6)
1. **Model Replikasi**:
   - `oneway`: Replikasi satu arah dari akar ke seluruh daun (*Point-to-Multipoint*).
   - `twoway`: Replikasi dua arah (anggota daun dapat merespons kembali ke akar).
   - `nway`: Komunikasi multi-arah penuh antar semua anggota (*Full-Mesh* dengan aturan *split-horizon* pencegah *echo*).
2. **Aturan Kepemilikan Endpoint**: Pasangan `(port, dlci)` multicast bersifat eksklusif dan tidak boleh tumpang tindih dengan titik masuk PVC point-to-point.
3. **Semantik Keaktifan LMI**: Status aktif sirkuit multicast ditentukan secara hierarkis berdasarkan ketersediaan endpoint akar (*root*) dan anggota daun (*leaves*).

---

## 6. Local Management Interface (`lmi`)

### 6.1 Tata Bahasa BNF
```bnf
<lmi-stmt> ::= "lmi" <port-name> ["dce"] [<lmi-type>] ["t392=" <time>] ["n392=" <number>] ["n393=" <number>] ["async=" ("true" | "false")]
             | "lmi" <port-name> "dte" [<lmi-type>] ["t391=" <time>] ["n391=" <number>] ["n392=" <number>] ["n393=" <number>] ["async=" ("true" | "false")]

<lmi-type> ::= "q933a" | "ansi" | "cisco" | "none"
```

### 6.2 Perbandingan Protokol LMI
| Protokol | Standar Acuan | DLCI Pengelolaan | Protocol Discriminator | Header & Enkapsulasi |
| :--- | :--- | :--- | :--- | :--- |
| **`q933a`** | ITU-T Q.933 Annex A / X.36 §11 | DLCI `0` | `0x08` | Unnumbered Information (UI), Codeset 0, mendukung segmentasi status Annex G/C. |
| **`ansi`** | ANSI T1.617 Annex D | DLCI `0` | `0x08` | UI frame, Codeset 5 untuk IE PVC status. |
| **`cisco`** | Consortium Gang of Four | DLCI `1023` | `0x09` | UI frame, Codeset 0. |
| **`none`** | - | - | - | Menonaktifkan LMI pada port tersebut. |

### 6.3 Ketentuan Bidirectional LMI pada NNI
Pada antarmuka NNI (`nni*/*`), polling DTE otomatis diaktifkan dan diwajibkan per **ITU-T X.76 §11.4** (*Bidirectional LMI*), di mana kedua ujung switch bertindak sebagai DCE dan DTE secara simetris.

---

## 7. Protokol Data Link LAPF ITU-T Q.922 (`lapf`)

### 7.1 Tata Bahasa BNF
```bnf
<lapf-stmt> ::= "lapf" <port-name> ["sabme=" ("active" | "passive" | "bidirectional")]
                ["xid=" ("active" | "passive" | "disable")]
                ["k=" <number>] ["n200=" <number>] ["n201=" <number>]
                ["t200=" <time>] ["t203=" <time>]
```

### 7.2 Parameter & Dinamika Jendela LAPF
- **`sabme` Startup Mode**: `active` (segera mengirim frame SABME saat boot), `passive` (menunggu SABME masuk), atau `bidirectional`.
- **`xid` Negotiation Mode**: Mengatur pertukaran parameter data link XID (Group ID `0x80`) per ITU-T Q.922 §5.4.
- **Kalkulasi Dynamic Initial Window** (ITU-T X.36 Appendix VII):
  $$k = 2 + \left\lfloor \frac{T_{td} \times R_u}{4 \times L_d} \right\rfloor$$
- **Skalasi Jendela Kemacetan $V(k)$** (ITU-T Q.922 Appendix I):
  - Saat menerima BECN: $V(k) \leftarrow \lfloor 0.625 \times V(k) \rfloor$.
  - Saat frame hilang / REJ: $V(k) \leftarrow \lfloor 0.25 \times V(k) \rfloor$.
  - Pemulihan bertahap (*Congestion Recovery*): $V(k) \leftarrow V(k) + 1$ setiap $N_w = 5$ I-frame sukses tanpa kemacetan.

---

## 8. Persinyalan & Layanan Panggilan Dinamis SVC (`svc int`, `svc addr`, `svc route`, `svc mcast`)

### 8.1 Inisialisasi Antarmuka SVC (`svc int`)
```ini
svc int <port-name> [dlci_low=<number>] [dlci_high=<number>] \
        [t303=<s>] [t305=<s>] [t308=<s>] [t310=<s>] [t301=<s>] [t316=<s>] [t317=<s>] [t322=<s>] \
        [cirdef=<bps>] [bcdef=<bits>] [bedef=<bits>] [fmifdef=<octets>] \
        [ftpdef=<0-15>] [fdpdef=<0-7>] [clsdef=<0-3>] \
        [revchg={allow | deny} | {ogrevchg={allow | deny} [icrevchg={allow | deny}]}]
```
- Menentukan pool alokasi DLCI dinamis (`dlci_low` default `512`, `dlci_high` default `991`).
- Menetapkan nilai QoS bawaan saat DTE tidak menyertakan IE LLCORE.

### 8.2 Registrasi Nomor Terminal Pelanggan (`svc addr`)
```ini
# Mode Manual (Mendukung Makro Mnemonik D=DNIC, G=SGC, E=SIC):
svc addr <port-name> manual [x121 | e164] <primary-number> [alias [x121 | e164] <alias-number>]

# Mode Autoprefix (Menyisipkan Prefiks DNIC+SGC+SIC Switch Secara Otomatis):
svc addr <port-name> autoprefix [x121 | e164] <subscriber-number> [alias [x121 | e164] <alias-number>]

# Mode Autonumber (Alokasi Sekuensial Multi-Prefix Otomatis):
svc addr <port-name> autonumber [x121 | e164] {global | group | global-reverse | group-reverse}
```
- **Penolakan Nomor Tak Terdaftar**: Panggilan yang ditujukan ke nomor yang belum terdaftar langsung ditolak dengan pesan `RELEASE COMPLETE` ber-Cause IE `#1: Unallocated number` atau Cause `#3: No route to destination` per ITU-T Q.850.

### 8.3 Tabel Perutean NNI SVC (`svc route`)
```ini
# Perutean Struktural X.121:
svc route <port-name> x121 {dcc=<nnn> [nd={<n> | none}] | dnic=<nnnn>} [pnic=<number>] [ind=<number>] [sgc=<number>] [sic=<number>]

# Perutean Struktural E.164:
svc route <port-name> e164 cc=<number> {[ic=<number>] | [ndc=<number> [area=<number>]]}

# Perutean Prefix Longest Prefix Match (LPM):
svc route <port-name> prefix {x121 | e164} <prefix-regex>
```
- Mesin perutean menerapkan **10-way Radix Trie** (`vfr_digit_node_t`) untuk pencocokan prefiks tercepat (*Longest Prefix Match*) dengan kompleksitas $O(L)$ terhadap panjang nomor.

---

## 9. Soft Permanent Virtual Circuits (`spvc`)

### 9.1 Tata Bahasa BNF
```bnf
<spvc-stmt> ::= "spvc" "pvc-link" <port-1> <dlci-1> ["lspvcid=" <number>] <dest-vfrs-number>
                "tgt=" ("specific" | "correlator") ("tdlci=" <number> | "tspvcid=" <number>)
                [<spvc-qos-params>] ["retry=" <time>]
```

### 9.2 Arsitektur & Penanganan Status Akses PVC
- Menjembatani segmen *Access PVC* lokal melintasi jaringan inti SVC NNI per **ITU-T X.76 Annex A**.
- **Pemantauan Status Sinkron**: Ketika sirkuit access PVC lokal mengalami *down* (terdeteksi via LMI), VFRS segera mengirim pesan `DISCONNECT` / `RELEASE` ber-Cause IE `#39: Permanent Frame Relay link out of service`.
- **Auto-Dial Restoral**: Ketika link access PVC kembali normal, VFRS secara otomatis menginisiasi panggilan `SETUP` ulang ke remote switch.

---

## 10. Inter-Switch Signaling & Management Protocol (`issmp`)

### 10.1 Tata Bahasa BNF
```bnf
<issmp-stmt> ::= "issmp" <port-name> ("enable" | "disable")
                 "as=" <number> "isic=" <number> "cost=" <number> ["pw=" <string>]
```

### 10.2 Arsitektur Protokol ISSMP
> [!WARNING]
> Modul protokol ISSMP saat ini masih berstatus *draft / reserved specification* (lihat [`vfrns_issmp_protocol_spec.md`](file:///c:/Users/rizki/Programming/vfrns/vfrns_issmp_protocol_spec.md)).

ISSMP dirancang sebagai protokol hibrida antar-switch virtual dalam ekosistem VFRNS yang memadukan:
1. **Telephony & ISDN Signaling (SS7 ISUP/MTP3 + DSS1/DSS2 Q.931/Q.932/Q.933 + ITU-T X.76)**: Untuk pembentukan koneksi sirkuit multi-hop, negosiasi kapabilitas switch, dan sinkronisasi status panggilan.
2. **Dynamic IP Routing Protocols (BGP + OSPF + RIP)**: Untuk pertukaran metrik link, topologi *Autonomous System* (`as=<number>`), propagasi status link switch (`isic=<number>`), dan konvergensi jalur terpendek.
3. **Native Frame Relay Transport**: Berjalan secara *native* di atas Frame Relay pada **DLCI 1015** dengan Network Layer Protocol Identifier **NLPID `0x8F`** (*Private Network Layer Protocols*), memanfaatkan frame LAPF tipe I (*Information*) dan tipe U (*Unnumbered*) pada lapisan DL-CORE dan DL-CONTROL.

---

## 11. Manajemen Kemacetan & Notifikasi CLLM (`cgst`)

### 11.1 Tata Bahasa BNF
```bnf
<cgst-stmt> ::= "cgst" <port-name> "rate=" <fps> ["clear=" <fps>] ["threshold=" <number>]
                ["cllm=" ("enable" | "disable") ["txint=" <time>]]
```

### 11.2 Mekanisme & Respons Kemacetan Bertingkat
1. **Kondisi Normal (Region I)**: Seluruh frame diteruskan normal sesuai kontrak QoS.
2. **Kemacetan Ringan s.d. Sedang (Region II)**:
   - Bit **FECN** disetel `1` pada frame arah maju (menuju node tujuan).
   - Bit **BECN** disetel `1` pada frame arah berlawanan (menuju node sumber).
3. **Kemacetan Sedang s.d. Berat (Region III)**:
   - Frame dengan bit **DE = 1** langsung dibuang (*discarded*).
   - Frame notifikasi CLLM XID dikirimkan secara periodik pada DLCI 1007 ke antarmuka yang mengalami kemacetan ($T_x = 5 \dots 30\text{s}$).
4. **Aturan Transparansi Non-Clearing (ITU-T X.36 §9.3.3)**: Switch **dilarang mereset** bit FECN, BECN, atau DE yang bernilai 1 kembali menjadi 0.

---

## 12. Perekaman Paket Jaringan Live PCAP (`capture`)

### 12.1 Tata Bahasa BNF
```bnf
<capture-stmt> ::= "capture" (<port-name> | <group-name> | "global" | "all") <file-path> ["svccap=" ("iframe" | "uiframe")]
```

### 12.2 Spesifikasi Format Rekaman
- Menghasilkan berkas format PCAP standar dengan *Data Link Type* `LINKTYPE_FRELAY` (DLT 107).
- Dapat dibuka langsung dan dianalisis secara mendalam menggunakan **Wireshark** untuk inspeksi header Q.922, LMI, Q.933, dan payload protokol lapisan atas.
