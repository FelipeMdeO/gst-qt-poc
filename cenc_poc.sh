#!/usr/bin/env bash
# cenc_poc.sh — PoC: fragment -> encrypt (mp4encrypt, fallback MP4Box) -> write-keys-for-cencdec -> decrypt (optional) -> play
# Usage: ./cenc_poc.sh input.mp4
set -euo pipefail

INPUT="${1:-}"
if [ -z "$INPUT" ]; then
  echo "Usage: $0 <input.mp4>"
  exit 2
fi

# Tools used
REQUIRED_CMDS=(mp4fragment mp4encrypt mp4decrypt mp4dump mp4info gst-launch-1.0 ffmpeg sha256sum xxd MP4Box)
for cmd in "${REQUIRED_CMDS[@]}"; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "[ERROR] Required tool '$cmd' not found in PATH. Please install it and retry."
    exit 3
  fi
done

# Filenames
BASE="$(basename "$INPUT" .mp4)"
WORKDIR="$(pwd)"
FRAG="${WORKDIR}/${BASE}_frag.mp4"
ENCRYPTED="${WORKDIR}/${BASE}_encrypted.mp4"
DECRYPTED="${WORKDIR}/${BASE}_decrypted.mp4"
LOG_ENC="${WORKDIR}/${BASE}_mp4encrypt.out"
LOG_ENC_TRY="${WORKDIR}/${BASE}_mp4encrypt_try.out"
KEYFILE="${WORKDIR}/${BASE}_keys.txt"   # text file 1:<hex> 2:<hex>
TMP_KEY_PREFIX="/tmp"

# Fixed test key (you may change these if you want)
USE_FIXED_KEY=${USE_FIXED_KEY:-0}
FIXED_KEY_HEX="${FIXED_KEY_HEX:-b29ac3cfbef6519a1986feb4547c39be}"
FIXED_KEY_IV="0000000000000000"

echo "---- CENC PoC Script (MP4Box fallback) ----"
echo "Input file: $INPUT"
echo "Working dir: $WORKDIR"
echo

# Step 1: fragment (or remux)
echo "[STEP 1] Ensure fragmented MP4 (required for CENC)"
echo " * Running mp4fragment on input..."
if mp4fragment "$INPUT" "$FRAG" 2>&1 | tee /dev/stderr | grep -qi "error"; then
  echo "[WARN] mp4fragment reported an error. Attempting remux via ffmpeg..."
  rm -f "$FRAG" || true
  ffmpeg -y -i "$INPUT" -c copy -movflags +frag_keyframe+empty_moov "$FRAG"
fi
echo " * Fragment check:"
mp4info "$FRAG" | sed -n '1,120p' | sed 's/^/   /'
echo

# helper: run mp4encrypt and capture
run_mp4encrypt_capture() {
  local cmd=("$@")
  echo " * Running: ${cmd[*]}"
  if "${cmd[@]}" 2>&1 | tee "$LOG_ENC_TRY"; then
    return 0
  else
    return 1
  fi
}

# Step 2: try mp4encrypt (fixed key if requested)
echo "[STEP 2] Try mp4encrypt (preferred)"
ENCRYPT_OK=0
MP4ENCRYPT_WARNINGS=0

if [ "$USE_FIXED_KEY" -eq 1 ]; then
  echo " * Using fixed key: $FIXED_KEY_HEX"
  if run_mp4encrypt_capture mp4encrypt --method MPEG-CENC \
       --key 1:${FIXED_KEY_HEX}:${FIXED_KEY_IV} \
       --key 2:${FIXED_KEY_HEX}:${FIXED_KEY_IV} \
       "$FRAG" "$ENCRYPTED"; then
    ENCRYPT_OK=1
  else
    ENCRYPT_OK=0
  fi
else
  if run_mp4encrypt_capture mp4encrypt --method MPEG-CENC "$FRAG" "$ENCRYPTED"; then
    ENCRYPT_OK=1
  else
    ENCRYPT_OK=0
  fi
fi

# persist attempt log
mv -f "$LOG_ENC_TRY" "$LOG_ENC" || true

# inspect mp4encrypt warnings that indicate \"will not be encrypted\"
if grep -E "will not be encrypted" "$LOG_ENC" >/dev/null 2>&1; then
  MP4ENCRYPT_WARNINGS=1
fi

# If mp4encrypt succeeded and produced encryption boxes, good.
if [ "$ENCRYPT_OK" -eq 1 ] && mp4dump "$ENCRYPTED" 2>/dev/null | egrep -i "tenc|senc|pssh|default_KID" -n >/dev/null; then
  echo " * mp4encrypt succeeded and encryption boxes present."
else
  echo "[WARN] mp4encrypt did not produce encryption boxes or failed."
  # Attempt fallback: use MP4Box -crypt
  echo "[STEP 2.FALLBACK] Attempting MP4Box -crypt fallback"

  # determine keys to use:
  if [ "$USE_FIXED_KEY" -eq 1 ]; then
    KEY1="$FIXED_KEY_HEX"
    KEY2="$FIXED_KEY_HEX"
  else
    # if mp4encrypt printed KEY.1/2 in log, extract them
    KEY1=$(grep -Eo "KEY.1=[0-9A-Fa-f]+" "$LOG_ENC" 2>/dev/null | sed 's/KEY.1=//' | head -n1 || true)
    KEY2=$(grep -Eo "KEY.2=[0-9A-Fa-f]+" "$LOG_ENC" 2>/dev/null | sed 's/KEY.2=//' | head -n1 || true)
    # if still empty, generate random keys (we will persist them in KEYFILE)
    if [ -z "$KEY1" ]; then
      echo " * No KEY1 from mp4encrypt log; generating random key1"
      KEY1=$(openssl rand -hex 16 2>/dev/null || (xxd -l 16 -p /dev/urandom))
    fi
    if [ -z "$KEY2" ]; then
      echo " * No KEY2 from mp4encrypt log; generating random key2"
      KEY2=$(openssl rand -hex 16 2>/dev/null || (xxd -l 16 -p /dev/urandom))
    fi
  fi

  # generate random KIDs for MP4Box mapping
  KID1=$(openssl rand -hex 16 2>/dev/null || (xxd -l 16 -p /dev/urandom))
  KID2=$(openssl rand -hex 16 2>/dev/null || (xxd -l 16 -p /dev/urandom))
  CRYPT_XML="${WORKDIR}/${BASE}_crypt.xml"
  echo " * Writing crypt.xml -> $CRYPT_XML"
  cat > "$CRYPT_XML" <<EOF
<Crypto>
  <SchemeInfo trackID="1" trackType="video" scheme="cenc">
    <key KID="${KID1}" value="${KEY1}"/>
  </SchemeInfo>
  <SchemeInfo trackID="2" trackType="audio" scheme="cenc">
    <key KID="${KID2}" value="${KEY2}"/>
  </SchemeInfo>
</Crypto>
EOF

  echo " * Running MP4Box -crypt..."
  if MP4Box -crypt "$CRYPT_XML" "$FRAG" -out "$ENCRYPTED" 2>&1 | tee "${WORKDIR}/${BASE}_mp4box.out"; then
    echo " * MP4Box -crypt produced: $ENCRYPTED"
    # update KEYFILE and TMP key files for cencdec
    echo " * Writing keyfile: $KEYFILE"
    {
      printf "1:%s\n" "$KEY1"
      printf "2:%s\n" "$KEY2"
    } > "$KEYFILE"
    chmod 600 "$KEYFILE" || true

    # write binary key files named by KID (for cencdec)
    echo -n "$KEY1" | tr -d ' \n\r' | xxd -r -p > "${TMP_KEY_PREFIX}/${KID1}.key"
    chmod 600 "${TMP_KEY_PREFIX}/${KID1}.key" || true
    echo -n "$KEY2" | tr -d ' \n\r' | xxd -r -p > "${TMP_KEY_PREFIX}/${KID2}.key"
    chmod 600 "${TMP_KEY_PREFIX}/${KID2}.key" || true

    echo "   -> wrote ${TMP_KEY_PREFIX}/${KID1}.key and ${TMP_KEY_PREFIX}/${KID2}.key"
    ENCRYPT_OK=1
  else
    echo "[ERROR] MP4Box -crypt fallback failed. See ${WORKDIR}/${BASE}_mp4box.out"
    exit 6
  fi
fi

# If still not encrypted, abort
if [ "$ENCRYPT_OK" -ne 1 ]; then
  echo "[ERROR] Unable to produce an encrypted MP4 file. Aborting."
  exit 7
fi

# Step 2.1: ensure we have KEY1/KEY2 variables (if we used mp4encrypt earlier and keys were printed)
if [ -z "${KEY1:-}" ] || [ -z "${KEY2:-}" ]; then
  # attempt to extract from KEYFILE if created
  if [ -f "$KEYFILE" ]; then
    KEY1=$(grep -Eo "^1:[0-9A-Fa-f]+" "$KEYFILE" | sed 's/1://' || true)
    KEY2=$(grep -Eo "^2:[0-9A-Fa-f]+" "$KEYFILE" | sed 's/2://' || true)
  fi
fi

# Final verification: ensure encrypted file has boxes
echo "[STEP 2.2] Inspect encrypted file (tenc/senc/pssh/default_KID)"
mp4dump "$ENCRYPTED" | egrep -i "tenc|senc|pssh|default_KID" -n || true
mp4info "$ENCRYPTED" | sed -n '1,200p' | sed 's/^/   /'
echo

# Step 3: decrypt using captured keys (optional round-trip)
echo "[STEP 3] Decrypting using captured keys (round-trip verification)"
if [ -z "${KEY1:-}" ] || [ -z "${KEY2:-}" ]; then
  echo "[WARN] No explicit keys available to run mp4decrypt. Skipping round-trip decrypt."
else
  echo " * Running mp4decrypt --key 1:${KEY1} --key 2:${KEY2} ${ENCRYPTED} ${DECRYPTED}"
  mp4decrypt --key 1:"${KEY1}" --key 2:"${KEY2}" "${ENCRYPTED}" "${DECRYPTED}" 2>&1 | tee /dev/stderr || true
  if [ -f "$DECRYPTED" ]; then
    echo " * Decrypted file: $DECRYPTED"
    ls -lh "$DECRYPTED" || true
    echo "[STEP 3.1] sha256"
    sha256sum "$INPUT" "$DECRYPTED" || true
  else
    echo "[WARN] mp4decrypt did not produce a decrypted file; check keys and output"
  fi
fi

# Step 4: optional play
echo "[STEP 4] Play decrypted file (optional)"
if [ -f "$DECRYPTED" ]; then
  gst-launch-1.0 filesrc location="$DECRYPTED" ! qtdemux ! decodebin ! autovideosink || true
else
  echo "[INFO] No decrypted file; to play encrypted file with cencdec ensure /tmp/<KID>.key exists for default_KID in the container."
fi

echo
echo "---- Done ----"
