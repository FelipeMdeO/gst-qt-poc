#!/usr/bin/env bash
# cenc_poc_auto.sh — PoC: fragment -> encrypt -> decrypt -> play
# Usage: ./cenc_poc_auto.sh input.mp4
# Output: log to terminal step-by-step (English)

set -euo pipefail

INPUT="${1:-}"
if [ -z "$INPUT" ]; then
  echo "Usage: $0 <input.mp4>"
  exit 2
fi

# Tools used
REQUIRED_CMDS=(mp4fragment mp4encrypt mp4decrypt mp4dump mp4info gst-launch-1.0 ffmpeg sha256sum)
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
KEYFILE="${WORKDIR}/${BASE}_keys.txt"   # <-- new artifact: keys output

# Fixed test key (you may change these if you want)
# NOTE: if you prefer to force a specific key, set USE_FIXED_KEY=1
USE_FIXED_KEY=${USE_FIXED_KEY:-0}
FIXED_KEY_HEX="${FIXED_KEY_HEX:-b29ac3cfbef6519a1986feb4547c39be}"
FIXED_KEY_IV="0000000000000000"    # common test IV; adjust if needed

echo "---- CENC PoC Script (updated) ----"
echo "Input file: $INPUT"
echo "Working dir: $WORKDIR"
echo

# Step 1: fragment or remux input so we have fragmented MP4
echo "[STEP 1] Ensure fragmented MP4 (required for CENC)"
echo " * Running mp4fragment on input..."
if mp4fragment "$INPUT" "$FRAG" 2>&1 | tee /dev/stderr | grep -qi "error"; then
  echo "[WARN] mp4fragment reported an error. Attempting remux via ffmpeg..."
  rm -f "$FRAG" || true
  ffmpeg -y -i "$INPUT" -c copy -movflags +frag_keyframe+empty_moov "$FRAG"
fi

echo " * Checking fragment status:"
mp4info "$FRAG" | sed -n '1,120p' | sed 's/^/   /'
echo

# Helper: check if file contains encryption boxes
file_has_encryption() {
  local f="$1"
  # look for boxes related to CENC
  if mp4dump "$f" 2>/dev/null | egrep -i "tenc|senc|pssh|default_KID" -n >/dev/null; then
    return 0
  fi
  return 1
}

# Helper: run mp4encrypt and capture stdout/stderr into LOG_ENC
run_mp4encrypt_capture() {
  local cmd=("$@")
  echo " * Running: ${cmd[*]}"
  # run and capture
  if "${cmd[@]}" 2>&1 | tee "$LOG_ENC_TRY"; then
    return 0
  else
    return 1
  fi
}

# Step 2: encrypt with mp4encrypt
echo "[STEP 2] Encrypt (CENC) — trying fixed key (if configured) then fallback to autogen"

ENCRYPT_OK=0

if [ "$USE_FIXED_KEY" -eq 1 ]; then
  echo " * Trying fixed key encryption using key: $FIXED_KEY_HEX (with IV $FIXED_KEY_IV)"
  # Most mp4encrypt variants accept: --key <track>:<key>:<iv>
  if run_mp4encrypt_capture mp4encrypt --method MPEG-CENC \
       --key 1:${FIXED_KEY_HEX}:${FIXED_KEY_IV} \
       --key 2:${FIXED_KEY_HEX}:${FIXED_KEY_IV} \
       "$FRAG" "$ENCRYPTED"; then
    ENCRYPT_OK=1
  else
    echo "[WARN] mp4encrypt returned non-zero. See $LOG_ENC_TRY"
    ENCRYPT_OK=0
  fi
fi

# If not using fixed key, or fixed-key attempt failed, try autogen
if [ "$ENCRYPT_OK" -eq 0 ]; then
  echo " * Attempting to encrypt (autogen keys) — capturing printed KEY.1/KEY.2"
  if run_mp4encrypt_capture mp4encrypt --method MPEG-CENC "$FRAG" "$ENCRYPTED"; then
    ENCRYPT_OK=1
  else
    echo "[ERROR] mp4encrypt failed (autogen attempt). See $LOG_ENC_TRY"
    echo "       Aborting to avoid operating on an unencrypted file."
    cat "$LOG_ENC_TRY" >&2 || true
    exit 4
  fi
fi

# Move last try log into main log file
mv -f "$LOG_ENC_TRY" "$LOG_ENC" || true

# Quick check: fail if mp4encrypt printed obvious error text
if grep -Ei "error|invalid argument|usage" "$LOG_ENC" >/dev/null 2>&1; then
  echo "[ERROR] mp4encrypt reported an error. See $LOG_ENC"
  exit 5
fi

# Verify encrypted file contains encryption boxes
if file_has_encryption "$ENCRYPTED"; then
  echo " * Encryption boxes found in $ENCRYPTED"
else
  echo "[ERROR] Encrypted file does not contain expected encryption boxes (tenc/senc/pssh/default_KID)."
  echo "        Inspect $LOG_ENC and the produced file with mp4dump. Aborting."
  mp4dump "$ENCRYPTED" | egrep -i "tenc|senc|pssh|default_KID" -n || true
  exit 6
fi

# Step 2.1: extract keys if printed (autogen case)
echo
echo "[STEP 2.1] Check mp4encrypt output for KEY.1 / KEY.2"
KEY1=""
KEY2=""
if grep -E "KEY.1|KEY.2" "$LOG_ENC" >/dev/null 2>&1; then
  KEY1=$(grep -Eo "KEY.1=[0-9A-Fa-f]+" "$LOG_ENC" | sed 's/KEY.1=//g' | head -n1 || true)
  KEY2=$(grep -Eo "KEY.2=[0-9A-Fa-f]+" "$LOG_ENC" | sed 's/KEY.2=//g' | head -n1 || true)
  echo " * Found keys from mp4encrypt output (if any):"
  echo "    KEY.1 = ${KEY1:-<not printed>}"
  echo "    KEY.2 = ${KEY2:-<not printed>}"
fi

# If fixed key was used and no printed keys, assume fixed values
if [ "$USE_FIXED_KEY" -eq 1 ] && [ -z "${KEY1:-}" ]; then
  echo " * No KEY.1/KEY.2 printed — using fixed key variables provided by caller."
  KEY1="$FIXED_KEY_HEX"
  KEY2="$FIXED_KEY_HEX"
fi

# If still no keys and encryption boxes exist, we cannot proceed with decryption
if [ -z "${KEY1:-}" ] || [ -z "${KEY2:-}" ]; then
  echo "[WARN] No explicit keys available. If mp4encrypt used a system key/DRM, you may not be able to decrypt locally."
  echo "       Inspect $LOG_ENC and the encrypted file with mp4dump to locate default_KID/pssh data."
  # Do not abort here automatically — user may choose to proceed. But we will abort to avoid false decrypts.
  exit 7
fi

# ----------------------------
# NEW: write keys to a file so the player (or other tools) can use them
# Format: one key per line: <track>:<hexkey>
# Example:
# 1:b29ac3cfbef6519a1986feb4547c39be
# 2:b29ac3cfbef6519a1986feb4547c39be
# ----------------------------
echo " * Writing keys to file: $KEYFILE (mode 600)"
{
  printf "1:%s\n" "$KEY1"
  printf "2:%s\n" "$KEY2"
} > "$KEYFILE"
chmod 600 "$KEYFILE" || true
echo "   -> $KEYFILE"

echo
echo "[STEP 2.2] Inspect encrypted file (tenc/senc/pssh/default_KID)"
mp4dump "$ENCRYPTED" | egrep -i "tenc|senc|pssh|default_KID" -n || true
mp4info "$ENCRYPTED" | sed -n '1,200p' | sed 's/^/   /'
echo

# Step 3: decrypt using captured keys
echo "[STEP 3] Decrypting using captured keys"
if [ -z "${KEY1:-}" ] || [ -z "${KEY2:-}" ]; then
  echo "[ERROR] No keys available to decrypt. Aborting."
  exit 10
fi

echo " * Running mp4decrypt ..."
mp4decrypt --key 1:"$KEY1" --key 2:"$KEY2" "$ENCRYPTED" "$DECRYPTED" 2>&1 | tee /dev/stderr

echo " * Decrypted file: $DECRYPTED"
ls -lh "$DECRYPTED" || true

echo
# Step 3.1: validate round-trip integrity
if [ -f "$DECRYPTED" ]; then
  echo "[STEP 3.1] Verifying round-trip integrity (sha256)"
  sha256sum "$INPUT" "$DECRYPTED" || true
  echo "(Compare hashes above; identical hashes indicate perfect round-trip.)"
else
  echo "[WARN] Decrypted file not found — skipping checksum verification."
fi

echo
# Step 4: Play decrypted file with GStreamer
echo "[STEP 4] Playing decrypted file with GStreamer (gst-launch-1.0)"
echo " * If you prefer not to play, press Ctrl+C to cancel now (3s)..."
sleep 3

gst-launch-1.0 filesrc location="$DECRYPTED" ! qtdemux ! decodebin ! autovideosink || true

echo
 echo "---- Done: PoC finished ----"
