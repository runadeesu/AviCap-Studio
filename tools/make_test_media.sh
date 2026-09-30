#!/usr/bin/env bash
# Generates small synthetic media files with known properties for the test
# suite (requires the ffmpeg CLI on the build host; the product itself links
# libav* directly and never shells out to ffmpeg).
#
#   tools/make_test_media.sh <output-dir>
set -euo pipefail
OUT="${1:?output dir}"
mkdir -p "$OUT"
FF="ffmpeg -hide_banner -loglevel error -y"

# 1) 1080p30 H.264 + stereo AAC 48 kHz, 5 s (golden path source).
[ -f "$OUT/av_1080p30.mp4" ] || $FF \
  -f lavfi -i "testsrc2=size=1920x1080:rate=30:duration=5" \
  -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=5" \
  -filter_complex "[1:a]aformat=channel_layouts=stereo[a]" -map 0:v -map "[a]" \
  -c:v libx264 -preset veryfast -g 30 -pix_fmt yuv420p -c:a aac -b:a 128k -shortest "$OUT/av_1080p30.mp4"

# 2) 29.97 fps 720p with frame numbers burned in (frame accuracy), 4 s.
[ -f "$OUT/ntsc_720p.mp4" ] || $FF \
  -f lavfi -i "testsrc=size=1280x720:rate=30000/1001:duration=4" \
  -c:v libx264 -preset veryfast -g 15 -bf 2 -pix_fmt yuv420p "$OUT/ntsc_720p.mp4"

# 3) Speech-like tone with silences: tone 0-1s, silent 1-2.5s, tone 2.5-4.5s,
#    short dip 4.5-4.7s, tone 4.7-6s, silent 6-8s, tone 8-9s. Mono 48 kHz.
[ -f "$OUT/speech_silence.wav" ] || $FF \
  -f lavfi -i "aevalsrc='0.5*sin(2*PI*300*t)*(lt(t,1)+between(t,2.5,4.5)+between(t,4.7,6)+between(t,8,9))':s=48000:d=9" \
  -c:a pcm_s16le "$OUT/speech_silence.wav"

# 4) Click track at 120 BPM (beat every 0.5 s), 8 s.
[ -f "$OUT/beats_120bpm.wav" ] || $FF \
  -f lavfi -i "aevalsrc='0.9*sin(2*PI*1000*t)*exp(-60*mod(t,0.5))':s=44100:d=8" \
  -c:a pcm_s16le "$OUT/beats_120bpm.wav"

# 5) Three hard scene cuts at 2 s and 4 s (red, green, blue with noise), 6 s.
[ -f "$OUT/scenes.mp4" ] || $FF \
  -f lavfi -i "color=c=red:size=640x360:rate=25:duration=2" \
  -f lavfi -i "color=c=green:size=640x360:rate=25:duration=2" \
  -f lavfi -i "color=c=blue:size=640x360:rate=25:duration=2" \
  -filter_complex "[0][1][2]concat=n=3:v=1:a=0,noise=alls=20:allf=t[v]" -map "[v]" \
  -c:v libx264 -preset veryfast -pix_fmt yuv420p "$OUT/scenes.mp4"

# 6) Still images.
[ -f "$OUT/still.png" ] || $FF -f lavfi -i "testsrc2=size=800x600:rate=1" -frames:v 1 "$OUT/still.png"
[ -f "$OUT/photo.jpg" ] || $FF -f lavfi -i "mandelbrot=size=1024x768:rate=1" -frames:v 1 "$OUT/photo.jpg"

# 7) Other codecs/containers.
[ -f "$OUT/vp9.webm" ] || $FF -f lavfi -i "testsrc=size=640x360:rate=24:duration=2" \
  -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -b:v 500k "$OUT/vp9.webm"
[ -f "$OUT/prores.mov" ] || $FF -f lavfi -i "testsrc=size=640x360:rate=25:duration=2" \
  -f lavfi -i "sine=frequency=880:sample_rate=48000:duration=2" -c:v prores_ks -profile:v 1 \
  -c:a pcm_s16le "$OUT/prores.mov"
[ -f "$OUT/mono_44k.mp3" ] || $FF -f lavfi -i "sine=frequency=660:sample_rate=44100:duration=3" \
  -c:a libmp3lame -b:a 128k "$OUT/mono_44k.mp3"
[ -f "$OUT/mpeg2.ts" ] || $FF -f lavfi -i "testsrc=size=720x480:rate=30000/1001:duration=2" \
  -f lavfi -i "sine=frequency=500:sample_rate=48000:duration=2" -c:v mpeg2video -c:a mp2 "$OUT/mpeg2.ts"

# 8) A clip whose file name is Japanese (Unicode path handling).
[ -f "$OUT/日本語クリップ.mp4" ] || cp "$OUT/ntsc_720p.mp4" "$OUT/日本語クリップ.mp4"

echo "Test media ready in $OUT"
