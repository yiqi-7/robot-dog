/**
 * 语音编解码（PCM ↔ Ogg/Opus）
 *
 * 抽成独立模块的原因：
 *   1. 端侧为省带宽把语音编成 Opus（3.6s：112KB → 5KB），网关必须能解回 PCM 再送 ASR
 *   2. 编解码依赖外部 ffmpeg，单独成模块才能脱离 MQTT 做确定性测试
 *
 * 语音是偶发事件（按键说话），临时文件往返足够快，比在进程内维持管道简单可靠。
 */
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";

function tmpPaths(tag) {
  return {
    inFile: path.join(os.tmpdir(), `qbot-voice-${tag}.ogg`),
    outFile: path.join(os.tmpdir(), `qbot-voice-${tag}.pcm`),
  };
}

/**
 * Ogg/Opus -> PCM s16le。
 *
 * 解码失败返回 null —— 调用方必须放弃这条音频，
 * 否则会把 Ogg 容器字节当成 PCM 采样送进 ASR，识别结果必然是噪声。
 */
export function decodeOpusToPcm(oggBuf, sampleRate = 16000, channels = 1) {
  const tag = `${Date.now()}-${Math.random().toString(36).slice(2, 8)}`;
  const { inFile, outFile } = tmpPaths(tag);
  try {
    fs.writeFileSync(inFile, oggBuf);
    const r = spawnSync(
      "ffmpeg",
      [
        "-hide_banner", "-loglevel", "error", "-y",
        "-i", inFile,
        "-f", "s16le", "-ar", String(sampleRate), "-ac", String(channels),
        outFile,
      ],
      { encoding: "utf8", timeout: 15000 },
    );
    if (r.error || r.status !== 0) {
      const why = r.error ? r.error.message : (r.stderr || "").trim().slice(0, 160);
      console.log(`[asr] Opus 解码失败：${why || `ffmpeg exit ${r.status}`}`);
      return null;
    }
    return fs.readFileSync(outFile);
  } catch (e) {
    console.log(`[asr] Opus 解码异常：${e.message}`);
    return null;
  } finally {
    for (const f of [inFile, outFile]) {
      try { fs.unlinkSync(f); } catch { /* 已不存在 */ }
    }
  }
}

/**
 * PCM s16le -> Ogg/Opus（主要供测试与工具使用；端侧在 C++ 里做同样的 ffmpeg 调用）。
 */
export function encodePcmToOpus(pcmBuf, sampleRate = 16000, channels = 1, bitrate = "16k") {
  const tag = `${Date.now()}-${Math.random().toString(36).slice(2, 8)}`;
  const pcmFile = path.join(os.tmpdir(), `qbot-enc-${tag}.pcm`);
  const oggFile = path.join(os.tmpdir(), `qbot-enc-${tag}.ogg`);
  try {
    fs.writeFileSync(pcmFile, pcmBuf);
    const r = spawnSync(
      "ffmpeg",
      [
        "-hide_banner", "-loglevel", "error", "-y",
        "-f", "s16le", "-ar", String(sampleRate), "-ac", String(channels),
        "-i", pcmFile,
        "-c:a", "libopus", "-b:a", bitrate, "-f", "ogg", oggFile,
      ],
      { encoding: "utf8", timeout: 15000 },
    );
    if (r.error || r.status !== 0) {
      const why = r.error ? r.error.message : (r.stderr || "").trim().slice(0, 160);
      console.log(`[voice] Opus 编码失败：${why || `ffmpeg exit ${r.status}`}`);
      return null;
    }
    return fs.readFileSync(oggFile);
  } catch (e) {
    console.log(`[voice] Opus 编码异常：${e.message}`);
    return null;
  } finally {
    for (const f of [pcmFile, oggFile]) {
      try { fs.unlinkSync(f); } catch { /* 已不存在 */ }
    }
  }
}
