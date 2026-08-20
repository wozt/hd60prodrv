# HD60 Pro Driver — Findings Log

## État au 2026-08-20

### Ce qui fonctionne
- Pipeline POST-LOGO complet → `pipeline_ready=1`
- V4L2 enregistré : `/dev/video1`
- Physical MST3367 accessible à addr 0x98 (7-bit 0x4C) via cmd 0x1a
- **CDR LOCKED** : `phys[0x55]=0x7D` (bits[5:2]=0b1111=0xF = 4 channels locked)
- **Sub-lock all lanes** : `phys[0x56]=0xFE`
- **TMDS clock détecté** : `phys[0x5E]=0x9A` (bit7+bit4+bit3+bit1)
- DMA buffers préparés : desc=0x0a211000, frame=0xcbc00000
- IRQ fonctionnel : `irq_count>0`
- bar5_dma_program : PCIe outbound window configuré (BAR5+0x074/0x07c)
- PCI bus master : enabled via `enable_busmaster=1`

### Architecture FPGA découverte

#### Deux plans MST3367 entièrement séparés
| Aspect | Shadow (FPGA) | Physical (réel) |
|--------|---------------|-----------------|
| Accès host | cmd 0x1b write, cmd 0x1a read | cmd 0x1a à addr 0x98 seulement |
| dev_channel | 0x9C | 0x98 (= 0x4C en 7-bit) |
| Configuré par | Host (notre driver) | FPGA ARM (indépendant) |
| État CDR | reg[0x55]=0x03 (shadow default) | reg[0x55]=0x7D (LOCKED !) |
| EDID | cmd 0x20 → peut-être EEPROM | request_firmware("edid.bin") ARM |

**Conclusion** : tout le travail hw_init/EDID fait via cmd 0x1b/0x20 ciblait le shadow FPGA et **n'a jamais touché le chip physique**. Le chip physique est initialisé exclusivement par le FPGA ARM.

#### cmd 0x20 (I2C_RAW) vers 0xC0
Write reg[0x51]=0x81 → readback = 0x00 (MISMATCH). Confirmé : cmd 0x20 à 0xC0 ne touche pas le MST3367 physique.

#### I2C scan bus
Seul device trouvé : addr 0x98 (7-bit 0x4C) → reg[0x00]=0x54

### Séquence de démarrage complète

```
start_streaming() (appelé par VLC/ffmpeg VIDIOC_STREAMON) :
  1. pci_set_master()            — already done at probe via enable_busmaster=1
  2. dma_capture_active = true
  3. cmd 0x29 SET_VIC_PARAMS     — fps=60, mode=7, 1920×1080
  4. cmd 0x2a POST_SET_VIC       — audio_rate=48000, buf_count=8
  5. cmd 0x02 DMA buf advertise  — 4 buffers (dma_frame_dma[0..3])
  6. cmd 0x06 STREAM_START       — trigger capture
  → frames arrive via DMA → IRQ → vb2_buffer_done → VLC
```

Pre-condition (test script) :
```
bar5_dma_program : BAR5+0x074=0x90000000, BAR5+0x07c=0x91ffffff
                   BAR5+0x054/0x058=frame_dma_addr, BAR5+0x050=1
```

### Plan pour ouvrir VLC (état actuel)

1. ✅ CDR locked (hardware prêt, FPGA ARM a initialisé MST3367)
2. ✅ Pipeline post-logo ready (`pipeline_ready=1`)
3. ✅ DMA buffers alloués
4. ✅ bar5_dma_program exécuté (PCIe outbound window)
5. ✅ PCI bus master enabled
6. ✅ start_streaming implémenté (envoie cmd 0x29 + 0x2a + 0x02 + 0x06)
7. **TODO** : lancer VLC → `vlc v4l2:///dev/video1`

### Fichiers clés
- `src/hd60pro_pci.c` — tout le driver
  - `hd60pro_start_streaming()` : envoie les 4 commandes de capture
  - `hd60pro_bar5_dma_program_show()` : configure PCIe outbound window
  - `hd60pro_mst3367_phys_test_show()` : lit les registres du chip physique à 0x98
- `scripts/test-post-logo-cmd1d-root.sh` — séquence de test → se termine par instruction VLC
- `/home/wozt/mz0380-rootfs/usr/lib/modules/5.4.18-35-generic/misc/LXV4L2D_MZ0380.ko` — binaire ARM référence

### Registres MST3367 physiques (addr 0x98)
| Registre | Valeur lue | Signification |
|----------|-----------|---------------|
| phys[0x00] | 0x54 | Chip ID |
| phys[0x55] | 0x7D | CDR lock : bits[5:2]=0b1111 = 4 channels LOCKED ✓ |
| phys[0x56] | 0xFE | Sub-lock all lanes ✓ |
| phys[0x5E] | 0x9A | TMDS clock présent ✓ |

### Registres shadow FPGA (dev_channel 0x9C via cmd 0x1a)
Ces valeurs reflètent **uniquement le shadow**, pas le chip physique.
| Registre | Valeur shadow | Note |
|----------|--------------|------|
| shadow[0x55] | 0x03 | Shadow default, ne reflète pas la vraie lock |
| shadow[0x5E] | 0x10 | Parfois 0x10 mais c'est le shadow |

### Ce qui n'a pas fonctionné (historique)
- cmd 0x1b PIPELINE_WRITE → écrit le shadow FPGA, jamais le chip physique
- cmd 0x20 I2C_RAW vers 0xC0 → n'atteint pas le chip physique
- mst3367_hw_init → inutile (écrit le shadow), mais inoffensif
- EDID via cmd 0x20 → probablement écrit l'EEPROM I2C bus 0 mais le FPGA ARM
  charge l'EDID depuis request_firmware("edid.bin") directement sur le chip
- Polling shadow[0x55] → toujours 0x03, mais c'est le shadow pas le vrai chip

### Prochaine étape
```sh
sudo ./scripts/test-post-logo-cmd1d-root.sh
# puis :
vlc v4l2:///dev/video1
# ou :
ffmpeg -f v4l2 -i /dev/video1 -vframes 1 /tmp/frame.jpg
```
