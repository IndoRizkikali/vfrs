# Catatan dan Masukan untuk implementasi layanan dan persinyalan SVC (Fase 1)

_Perlu diketahui, ini dibagi menjadi beberapa bagian berdasarkan tanggal, seiring dengan perjalanan waktu._

## Rabu, 05 Agustus 2026

1. Untuk pengalokasian DLCI, perlu diketahui bahwa DLCI yang dialokasikan dalam suatu panggilan antara antarmuka yang satu dengan antarmuka yang lain dapat **berbeda-beda ataupun sama**. Ini karena DLCI bersifat lokal (hanya berlaku untuk satu antarmuka saja), sehingga tidak perlu harus mengalokasikan DLCI yang berbeda (contoh: pada suatu panggilan, port UNI0/0 dengan DLCI 512 berpasangan dengan port UNI0/1 dengan DLCI 513, padahal, pada port UNI0/1, DLCI 512 tidak digunakan dan dapat dialokasikan ke panggilan tersebut) untuk setiap tautan SVC. Yang penting, VFRS dapat membedakan **tiap-tiap pasangan antarmuka + DLCI**, walaupun ada lebih dari satu antarmuka yang memiliki DLCI yang sama (contoh: karena alokasi DLCI adalah spesifik per-port, pada panggilan yang sama, port UNI0/0 dengan DLCI 512 dapat berpasangan dengan port UNI0/1 dengan DLCI 512 alih-alih DLCI 513).
2. Fungsi dari statement `alias=x121|e164,<alias_sub_number>` adalah untuk menyediakan layanan nomor alias/nomor tambahan. Untuk sekarang, layanan nomor alias ini disediakan dengan jalan mengubah nilai pada Called party number IE dan Connected number IE dari nomor alias (`<alias_sub_number>`) ke nomor utama (`<primary_sub_number>`).

## Kamis, 20 Agustus 2026

1. Calling party subaddress IE dan Called party subaddress IE harus diteruskan secara transparan antara antara DTE pemanggil dan DTE terpanggil.
2. 
