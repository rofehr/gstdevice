# vdr-plugin-gstreamer v0.2.0

GStreamer-basiertes Ausgabe-Plugin für den **VDR** (Video Disk Recorder).
Unterstützt H.264/H.265-Video, AAC-Audio, VA-API-Hardware-Dekodierung
und ein vollständiges OSD-Setup-Menü.

---

## Features

| Feature                    | Details                                              |
|----------------------------|------------------------------------------------------|
| **Video-Codecs**           | H.264 (HD), H.265/HEVC (UHD)                        |
| **Audio-Codecs**           | AAC, MP3                                             |
| **Hardware-Dekodierung**   | VA-API (Intel Quick Sync / AMD VCN)                 |
| **Software-Fallback**      | libav (avdec_h264, avdec_h265)                      |
| **OSD-Setup-Menü**         | Codec, HW-Decode, Sinks, Audio-Offset, Lautstärke   |
| **A/V-Sync Offset**        | ±500 ms, live anpassbar                             |
| **Lautstärke**             | 0–255, über VDR-Lautstärkeregelung                  |

---

## Abhängigkeiten

```bash
# Debian / Ubuntu
sudo apt-get install \
  vdr-dev \
  libgstreamer1.0-dev \
  libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad \
  gstreamer1.0-plugins-ugly \
  gstreamer1.0-libav \
  gstreamer1.0-vaapi
```

---

## Bauen & Installieren

```bash
make
sudo make install
```

Erzeugt `libvdr-gstreamer.so` und kopiert nach `/usr/lib/vdr/plugins/`.

---

## Laden

```bash
# /etc/vdr/conf.d/50-gstreamer.conf
-P gstreamer

# Mit explizitem Sink (überschreibt OSD-Einstellung beim Start):
-P "gstreamer --videosink=xvimagesink --audiosink=alsasink"
```

---

## OSD-Setup-Menü

Erreichbar über: **VDR → Einstellungen → Plugins → gstreamer**

```
--- Video ---
  Video Codec              [ H.264 (HD) | H.265 / HEVC (UHD) ]
  Hardware Decode (VA-API) [ Software | VA-API ]
  Active decoder:          h264parse  →  vaapih264dec
  Video Sink               autovideosink
--- Audio ---
  Audio Codec              [ AAC | MP3 ]
  Audio Sink               autoaudiosink
--- Sync & Volume ---
  Audio Offset (ms)        0          (-500 … +500)
  Volume                   255        (0 … 255)
--- Pipeline Info ---
  Video: vaapih264dec + vaapisink
  Audio: avdec_aac + autoaudiosink
```

Alle Einstellungen werden in `setup.conf` gespeichert und beim nächsten
Start automatisch geladen. Eine Codec- oder HW-Änderung löst einen
automatischen Pipeline-Neustart aus.

---

## Pipeline-Diagramme

### H.264 + VA-API (Standard)
```
VDR PlayTsVideo()
  appsrc → h264parse → vaapih264dec → vaapisink

VDR PlayTsAudio()
  appsrc → aacparse → avdec_aac → audioconvert
         → audioresample → identity(ts-offset) → autoaudiosink
```

### H.265 + VA-API
```
  appsrc → h265parse → vaapih265dec → vaapisink
```

### Software-Fallback
```
  appsrc → h264parse → avdec_h264 → videoconvert → autovideosink
```

---

## Architektur

```
cPluginGstreamer          Plugin-Klasse (Lifecycle, SetupParse, SetupMenu)
├─ cGstConfig             Einstellungen (global, in setup.conf persistent)
├─ cGstMenuSetup          OSD-Setup-Seite (cMenuSetupPage)
└─ cGstDevice             cDevice-Implementierung
   ├─ BuildPipeline()     Liest GstConfig, baut Video- & Audio-Branch
   ├─ CreateVideoElements() H.264/H.265 + VA-API oder SW
   ├─ CreateAudioElements() AAC/MP3 + identity(tsoffset)
   ├─ ApplyAudioOffset()  Setzt identity.ts-offset live
   ├─ ReconfigurePipeline() Reagiert auf OSD-Änderungen
   └─ BusSyncHandler()    Fehler/Warnung/EOS → syslog
```

---

## Konfigurationsschlüssel (setup.conf)

| Schlüssel        | Typ    | Wertebereich      | Standard         |
|------------------|--------|-------------------|------------------|
| `VideoCodec`     | int    | 0=H.264, 1=H.265  | 0                |
| `HardwareDecode` | int    | 0=SW, 1=VA-API    | 1                |
| `AudioCodec`     | int    | 0=AAC, 1=MP3      | 0                |
| `AudioOffset`    | int    | -500 … +500 (ms)  | 0                |
| `Volume`         | int    | 0 … 255           | 255              |
| `VideoSink`      | string | GStreamer-Element  | autovideosink    |
| `AudioSink`      | string | GStreamer-Element  | autoaudiosink    |

---

## Bekannte Einschränkungen / TODO

- [ ] Automatische Codec-Erkennung aus DVB-SI-Daten (PMT)
- [ ] HDMI-CEC über libCEC / GStreamer
- [ ] DVB-Untertitel (textoverlay)
- [ ] Teletext-Durchreichung
- [ ] NVIDIA NVDEC / V4L2-Unterstützung
- [ ] i18n / Übersetzungsdateien (.po)

---

## Lizenz

GPL v2 – entsprechend der VDR-Lizenz.
