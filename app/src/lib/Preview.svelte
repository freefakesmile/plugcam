<script lang="ts">
  // Live copy of what "Plugcam Camera" shows. Frames come as JPEG over a Tauri channel,
  // and only while the window is visible and the user has not hidden the preview.
  import { onMount } from "svelte";
  import { Video, LoaderCircle, Crop as CropIcon, Eye, EyeOff } from "@lucide/svelte";
  import { subscribePreview, onPreviewClear, setPreviewActive, setPreviewWidth } from "./api";
  import type { Crop } from "./api";
  import CropOverlay from "./CropOverlay.svelte";
  import { cropEdit, startCropEdit } from "./cropEdit.svelte";
  import { t } from "./i18n";

  let {
    streaming,
    cameraOn,
    live,
    empty,
    waiting,
    crop,
    frame,
    picture,
    hidden,
    onhidden,
  }: {
    streaming: boolean;
    cameraOn: boolean;
    /** Chip over the picture while streaming, e.g. "30 fps". */
    live: string;
    /** What the stage says with no picture and the camera off. */
    empty: { title: string; hint: string };
    /** What it says while the camera is on but no frame has come yet. */
    waiting: { title: string; hint: string };
    /** The saved crop, for the crop button. */
    crop: Crop;
    /** The camera frame's size. */
    frame: [number, number];
    /** The phone's picture as shown (after rotation); `null` while not streaming. */
    picture: [number, number] | null;
    /** The user hid the preview: no frames are made, the stage says so. */
    hidden: boolean;
    onhidden: (hidden: boolean) => void;
  } = $props();

  let canvas: HTMLCanvasElement;
  let hasFrame = $state(false);
  let drawing = false;

  onMount(() => {
    // A CPU-backed canvas: with GPU decoding of 40 JPEGs a second WebView2's GPU process holds
    // about 500 MB more memory for nothing.
    const ctx = canvas.getContext("2d", { willReadFrequently: true })!;
    subscribePreview(async (jpeg) => {
      if (drawing || hidden) return; // drop a frame rather than queue them
      drawing = true;
      try {
        const bitmap = await createImageBitmap(new Blob([jpeg], { type: "image/jpeg" }));
        if (canvas.width !== bitmap.width || canvas.height !== bitmap.height) {
          canvas.width = bitmap.width;
          canvas.height = bitmap.height;
        }
        ctx.drawImage(bitmap, 0, 0);
        bitmap.close();
        hasFrame = true;
      } finally {
        drawing = false;
      }
    });
    const unlisten = onPreviewClear(() => {
      ctx.clearRect(0, 0, canvas.width, canvas.height);
      hasFrame = false;
    });
    const onVisibility = () => setPreviewActive(!document.hidden);
    document.addEventListener("visibilitychange", onVisibility);

    // Ask for JPEGs as wide as the picture is on screen (it is letterboxed inside the canvas).
    let sentWidth = 0;
    const measure = () => {
      const aspect = canvas.width / canvas.height || 16 / 9;
      const shown = Math.min(canvas.clientWidth, canvas.clientHeight * aspect) * devicePixelRatio;
      const width = Math.ceil(shown / 16) * 16;
      if (width > 0 && width !== sentWidth) {
        sentWidth = width;
        setPreviewWidth(width);
      }
    };
    const sizer = new ResizeObserver(measure);
    sizer.observe(canvas);
    // Moving to a monitor with other scaling keeps the CSS size but changes the pixels.
    let scale = matchMedia(`(resolution: ${devicePixelRatio}dppx)`);
    const onScale = () => {
      scale.removeEventListener("change", onScale);
      scale = matchMedia(`(resolution: ${devicePixelRatio}dppx)`);
      scale.addEventListener("change", onScale);
      measure();
    };
    scale.addEventListener("change", onScale);
    return () => {
      sizer.disconnect();
      scale.removeEventListener("change", onScale);
      document.removeEventListener("visibilitychange", onVisibility);
      unlisten.then((f) => f());
    };
  });

  $effect(() => {
    if (!cameraOn || hidden) hasFrame = false;
  });
</script>

<div class="stage" class:dim={cameraOn && !streaming && hasFrame}>
  <canvas bind:this={canvas} width="960" height="540" class:hidden={!hasFrame}></canvas>

  {#if !hasFrame}
    <div class="empty">
      {#if cameraOn && hidden}
        <EyeOff size={32} aria-hidden="true" />
        <span class="title">{t("preview.hidden")}</span>
        <span class="hint">{t("preview.hidden.hint")}</span>
        <button class="show-btn" onclick={() => onhidden(false)}>
          <Eye size={16} aria-hidden="true" />{t("preview.show")}
        </button>
      {:else if cameraOn}
        <LoaderCircle size={28} class="spin" aria-hidden="true" />
        <span class="title">{waiting.title}</span>
        {#if waiting.hint}<span class="hint">{waiting.hint}</span>{/if}
      {:else}
        <Video size={32} aria-hidden="true" />
        <span class="title">{empty.title}</span>
        {#if empty.hint}<span class="hint">{empty.hint}</span>{/if}
      {/if}
    </div>
  {/if}

  {#if streaming && hasFrame}
    <div class="chip" aria-hidden="true"><span class="dot"></span>{live}</div>
    {#if !cropEdit.active}
      <div class="tools">
        <button class="tool-btn" title={t("preview.hide")} aria-label={t("preview.hide")} onclick={() => onhidden(true)}>
          <EyeOff size={18} />
        </button>
        <button class="tool-btn" title={t("crop.edit")} aria-label={t("crop.edit")} onclick={() => startCropEdit(crop)}>
          <CropIcon size={18} />
        </button>
      </div>
    {/if}
  {/if}

  {#if cropEdit.active}
    <CropOverlay
      frameWidth={frame[0]}
      frameHeight={frame[1]}
      pictureWidth={picture?.[0] ?? 0}
      pictureHeight={picture?.[1] ?? 0} />
  {/if}
</div>

<style>
  .stage {
    position: relative;
    height: 100%;
    min-height: 0;
    border-radius: 8px;
    overflow: hidden;
    background: var(--stage);
    outline: 1px solid var(--stage-outline);
    outline-offset: -1px;
  }
  canvas {
    position: absolute;
    inset: 0;
    width: 100%;
    height: 100%;
    object-fit: contain;
    display: block;
    transition-property: opacity;
    transition-duration: 200ms;
  }
  .hidden {
    visibility: hidden;
  }
  .dim canvas {
    opacity: 0.35;
  }
  .empty {
    position: absolute;
    inset: 0;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 8px;
    padding: 24px;
    text-align: center;
    color: var(--stage-text);
  }
  .title {
    margin-top: 4px;
    font-size: 16px;
    font-weight: 600;
    color: #fff;
    text-wrap: balance;
  }
  .hint {
    max-width: 36ch;
    font-size: 13px;
    text-wrap: pretty;
  }
  .empty :global(.spin) {
    animation: spin 0.9s linear infinite;
  }
  @keyframes spin {
    to {
      rotate: 360deg;
    }
  }
  @media (prefers-reduced-motion: reduce) {
    .empty :global(.spin) {
      animation: none;
    }
  }
  .chip {
    position: absolute;
    inset-block-start: 12px;
    inset-inline-start: 12px;
    display: flex;
    align-items: center;
    gap: 6px;
    height: 24px;
    padding: 0 10px 0 8px;
    border-radius: 12px;
    background: rgb(0 0 0 / 0.7);
    color: #fff;
    font-size: 12px;
    font-weight: 600;
    font-variant-numeric: tabular-nums;
  }
  .tools {
    position: absolute;
    inset-block-start: 12px;
    inset-inline-end: 12px;
    display: flex;
    gap: 8px;
  }
  .tool-btn {
    display: grid;
    place-items: center;
    width: 32px;
    height: 32px;
    border: 0;
    border-radius: 16px;
    background: rgb(0 0 0 / 0.7);
    color: #fff;
    cursor: pointer;
    opacity: 0;
    transition-property: opacity, scale, background-color;
    transition-duration: 150ms;
  }
  .stage:hover .tool-btn,
  .tool-btn:focus-visible {
    opacity: 1;
  }
  .tool-btn:hover {
    background: rgb(40 40 40 / 0.8);
  }
  .tool-btn:active {
    scale: 0.94;
  }
  .show-btn {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    margin-top: 8px;
    height: 30px;
    padding: 0 14px;
    border: 1px solid rgb(255 255 255 / 0.25);
    border-radius: 15px;
    background: rgb(255 255 255 / 0.08);
    color: #fff;
    font: inherit;
    font-size: 13px;
    cursor: pointer;
  }
  .show-btn:hover {
    background: rgb(255 255 255 / 0.16);
  }
  .dot {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    background: #f03a2e;
  }
</style>
