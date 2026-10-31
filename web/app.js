"use strict";

const WIDTH = 600;
const HEIGHT = 400;
const MAX_BYTES = 20 * 1024 * 1024;
const MAX_PIXELS = 24_000_000;

// Approximate preview colors, not calibrated measurements of the panel.
const PALETTE = [
  [21, 21, 21],
  [255, 255, 255],
  [229, 188, 53],
  [185, 64, 56],
  [56, 92, 149],
  [82, 116, 90],
];

const fileInput = document.querySelector("#file-input");
const chooseButton = document.querySelector("#choose-file");
const dropzone = document.querySelector("#dropzone");
const canvas = document.querySelector("#preview");
const emptyState = document.querySelector("#empty-state");
const fitSelect = document.querySelector("#fit");
const ditherSelect = document.querySelector("#dither");
const saveButton = document.querySelector("#save-preview");
const feedback = document.querySelector("#feedback");
const artworkName = document.querySelector("#artwork-name");
const artworkDetails = document.querySelector("#artwork-details");
const loginScreen = document.querySelector("#login-screen");
const loginForm = document.querySelector("#login-form");
const logoutButton = document.querySelector("#logout");
const addButton = document.querySelector("#add-to-gallery");
const collectionGrid = document.querySelector("#collection-grid");
const collectionCount = document.querySelector("#collection-count");
const collectionPreview = document.querySelector("#collection-preview");
const collectionEmpty = document.querySelector("#collection-empty");
const collectionListEmpty = document.querySelector("#collection-list-empty");
const collectionName = document.querySelector("#collection-current-name");
const collectionDetails = document.querySelector("#collection-current-details");
const galleryFeedback = document.querySelector("#gallery-feedback");
const slideshowButton = document.querySelector("#slideshow-toggle");
const slideshowInterval = document.querySelector("#slideshow-interval");
const slideshowStatus = document.querySelector("#slideshow-status");
const deleteDialog = document.querySelector("#delete-dialog");
const deleteDescription = document.querySelector("#delete-description");
const collectionPagination = document.querySelector("#collection-pagination");
const paginationSummary = document.querySelector("#pagination-summary");
const paginationRange = document.querySelector("#pagination-range");
const firstPageButton = document.querySelector("#page-first");
const previousPageButton = document.querySelector("#page-previous");
const nextPageButton = document.querySelector("#page-next");
const lastPageButton = document.querySelector("#page-last");
const pageNumbers = document.querySelector("#page-numbers");
const pageJumpForm = document.querySelector("#page-jump-form");
const pageJumpInput = document.querySelector("#page-jump");

const context = canvas.getContext("2d");

// Only the small processing canvas is read back into JavaScript.
const processingCanvas = document.createElement("canvas");
processingCanvas.width = WIDTH;
processingCanvas.height = HEIGHT;

const processingContext = processingCanvas.getContext("2d", {
  willReadFrequently: true,
});

let sourceImage = null;
let sourceName = "";
let latestRequest = 0;
let loadingImage = false;
let previewReady = false;
let addingArtwork = false;
let signedIn = false;
let sessionVersion = 0;
let nextArtworkId = 1;
let selectedArtworkId = null;
let pendingDeleteId = null;
let slideshowTimer = null;
let nextChangeAt = 0;
let collectionPage = 1;

// Processed snapshots only: originals are not retained in the demo collection.
// Reloading clears the collection. This will be replaced by the C++ backend.
const collection = [];
const ARTWORKS_PER_PAGE = 24;

function syncEditorControls() {
  const ready = signedIn && previewReady && !loadingImage;
  saveButton.disabled = !ready;
  addButton.disabled = !ready || addingArtwork;
  addButton.textContent = addingArtwork ? "Adding artwork…" : "Add to gallery ＋";
  fitSelect.disabled = !signedIn || loadingImage || !sourceImage;
  ditherSelect.disabled = fitSelect.disabled;
  chooseButton.disabled = !signedIn;
}

function setFeedback(message, isError = false) {
  feedback.textContent = message;
  feedback.classList.toggle("error", isError);
}

function nearestColor(red, green, blue) {
  let closest = PALETTE[0];
  let smallestDistance = Infinity;

  for (const color of PALETTE) {
    const distance =
      (red - color[0]) ** 2 +
      (green - color[1]) ** 2 +
      (blue - color[2]) ** 2;

    if (distance < smallestDistance) {
      smallestDistance = distance;
      closest = color;
    }
  }

  return closest;
}

function clamp(value) {
  return Math.max(0, Math.min(255, value));
}

function quantize(imageData, useDithering) {
  const pixels = imageData.data;

  // Two error rows avoid allocating a full-size floating-point image.
  let currentErrors = new Float32Array((WIDTH + 2) * 3);
  let nextErrors = new Float32Array((WIDTH + 2) * 3);

  for (let y = 0; y < HEIGHT; y++) {
    for (let x = 0; x < WIDTH; x++) {
      const pixelOffset = (y * WIDTH + x) * 4;
      const errorOffset = (x + 1) * 3;
      const channels = [];

      for (let channel = 0; channel < 3; channel++) {
        channels[channel] = clamp(
          pixels[pixelOffset + channel] +
            (useDithering ? currentErrors[errorOffset + channel] : 0)
        );
      }

      const color = nearestColor(...channels);

      for (let channel = 0; channel < 3; channel++) {
        pixels[pixelOffset + channel] = color[channel];

        if (useDithering) {
          const error = channels[channel] - color[channel];
          const offset = errorOffset + channel;

          currentErrors[offset + 3] += error * (7 / 16);
          nextErrors[offset - 3] += error * (3 / 16);
          nextErrors[offset] += error * (5 / 16);
          nextErrors[offset + 3] += error * (1 / 16);
        }
      }

      pixels[pixelOffset + 3] = 255;
    }

    [currentErrors, nextErrors] = [nextErrors, currentErrors];
    nextErrors.fill(0);
  }

  return imageData;
}

function renderPreview() {
  if (!sourceImage) return;

  const imageWidth = sourceImage.naturalWidth;
  const imageHeight = sourceImage.naturalHeight;
  const scaleFunction = fitSelect.value === "cover" ? Math.max : Math.min;
  const scale = scaleFunction(WIDTH / imageWidth, HEIGHT / imageHeight);
  const drawWidth = imageWidth * scale;
  const drawHeight = imageHeight * scale;

  // White also provides the background for transparent PNGs.
  processingContext.fillStyle = "#ffffff";
  processingContext.fillRect(0, 0, WIDTH, HEIGHT);
  processingContext.imageSmoothingEnabled = true;
  processingContext.imageSmoothingQuality = "high";

  processingContext.drawImage(
    sourceImage,
    (WIDTH - drawWidth) / 2,
    (HEIGHT - drawHeight) / 2,
    drawWidth,
    drawHeight
  );

  const imageData = processingContext.getImageData(0, 0, WIDTH, HEIGHT);
  const useDithering = ditherSelect.value === "floyd-steinberg";

  context.putImageData(quantize(imageData, useDithering), 0, 0);

  canvas.hidden = false;
  emptyState.hidden = true;
  previewReady = true;
  syncEditorControls();

  const treatment = useDithering ? "Dithered" : "Nearest color";
  artworkDetails.textContent =
    `${imageWidth} × ${imageHeight} original · ${treatment} · Six-color preview`;
}

async function loadFile(file) {
  if (!file || !signedIn) return;

  const request = ++latestRequest;
  loadingImage = false;
  syncEditorControls();

  if (!["image/jpeg", "image/png"].includes(file.type)) {
    setFeedback("Please choose a JPG or PNG image.", true);
    return;
  }

  if (file.size > MAX_BYTES) {
    setFeedback("Please choose an image smaller than 20 MB.", true);
    return;
  }

  loadingImage = true;
  syncEditorControls();
  setFeedback("Preparing your artwork…");

  const objectUrl = URL.createObjectURL(file);

  try {
    const image = new Image();
    image.src = objectUrl;
    await image.decode();

    // Prevent a slower previous selection from replacing a newer one.
    if (request !== latestRequest) return;

    if (
      image.naturalWidth === 0 ||
      image.naturalHeight === 0 ||
      image.naturalWidth * image.naturalHeight > MAX_PIXELS
    ) {
      throw new Error("Please choose an image with at most 24 megapixels.");
    }

    sourceImage = image;
    sourceName = file.name.replace(/\.[^.]+$/, "") || "Artwork";
    artworkName.textContent = sourceName;

    renderPreview();
    setFeedback("Preview ready. Your image has not left this browser.");
  } catch (error) {
    if (request !== latestRequest) return;

    const message =
      error instanceof Error && error.message.includes("24 megapixels")
        ? error.message
        : "This image could not be opened. Try another JPG or PNG.";

    setFeedback(message, true);
  } finally {
    URL.revokeObjectURL(objectUrl);

    if (request === latestRequest) {
      loadingImage = false;
      syncEditorControls();
    }
  }
}

chooseButton.addEventListener("click", () => fileInput.click());

fileInput.addEventListener("change", () => {
  void loadFile(fileInput.files[0]);
  fileInput.value = "";
});

for (const eventName of ["dragenter", "dragover"]) {
  dropzone.addEventListener(eventName, (event) => {
    event.preventDefault();
    dropzone.classList.add("dragging");
  });
}

dropzone.addEventListener("dragleave", (event) => {
  if (!dropzone.contains(event.relatedTarget)) {
    dropzone.classList.remove("dragging");
  }
});

dropzone.addEventListener("drop", (event) => {
  event.preventDefault();
  dropzone.classList.remove("dragging");

  const files = event.dataTransfer.files;

  if (files.length !== 1) {
    setFeedback("Please drop one image at a time.", true);
    return;
  }

  void loadFile(files[0]);
});

fitSelect.addEventListener("change", renderPreview);
ditherSelect.addEventListener("change", renderPreview);

saveButton.addEventListener("click", () => {
  if (!signedIn || !previewReady || loadingImage) return;

  const filename = sourceName.replace(/[^a-z0-9_-]/gi, "-");
  const currentSession = sessionVersion;

  canvas.toBlob((blob) => {
    if (!signedIn || currentSession !== sessionVersion) return;

    if (!blob) {
      setFeedback("Could not save the preview. Please try again.", true);
      return;
    }

    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");

    link.href = url;
    link.download = `${filename || "artwork"}-inkhibit-preview.png`;
    document.body.append(link);
    link.click();
    link.remove();

    // Give the browser time to start the download before releasing the URL.
    window.setTimeout(() => URL.revokeObjectURL(url), 1000);
  }, "image/png");
});

// Demo session: no password verification, persistence, or server requests.
loginForm.addEventListener("submit", (event) => {
  event.preventDefault();
  signedIn = true;
  sessionVersion++;
  loginForm.reset();
  loginScreen.hidden = true;
  document.body.classList.remove("locked");
  syncEditorControls();
  logoutButton.focus({ preventScroll: true });
  window.scrollTo(0, 0);
});

logoutButton.addEventListener("click", () => {
  signedIn = false;
  sessionVersion++;
  latestRequest++;
  loadingImage = false;
  sourceImage = null;
  sourceName = "";
  previewReady = false;
  pauseSlideshow();
  canvas.hidden = true;
  emptyState.hidden = false;
  context.clearRect(0, 0, WIDTH, HEIGHT);
  processingContext.clearRect(0, 0, WIDTH, HEIGHT);
  artworkName.textContent = "Untitled, for now";
  artworkDetails.textContent = "Your artwork will appear here.";
  setFeedback("Images stay in your browser.");
  galleryFeedback.textContent = "";
  syncEditorControls();
  document.body.classList.add("locked");
  loginScreen.hidden = false;
  document.querySelector("#login-name").focus();
  window.scrollTo(0, 0);
});

function intervalMilliseconds() {
  return Math.max(180, Number(slideshowInterval.value) || 300) * 1000;
}

function updateSlideshowStatus() {
  if (slideshowTimer === null) {
    slideshowStatus.textContent = collection.length < 2
      ? "Add two artworks to start a slideshow."
      : "Slideshow paused. Take your time.";
    return;
  }

  const remaining = Math.max(0, Math.ceil((nextChangeAt - Date.now()) / 1000));
  const minutes = Math.floor(remaining / 60);
  const seconds = String(remaining % 60).padStart(2, "0");
  slideshowStatus.textContent = `Playing in this browser · next piece in ${minutes}:${seconds}`;
}

function pauseSlideshow() {
  if (slideshowTimer !== null) {
    window.clearInterval(slideshowTimer);
    slideshowTimer = null;
  }

  slideshowButton.textContent = "Start slideshow";
  slideshowButton.setAttribute("aria-pressed", "false");
  updateSlideshowStatus();
}

function updateSelection() {
  const selected = collection.find((artwork) => artwork.id === selectedArtworkId);
  collectionPreview.hidden = !selected;
  collectionEmpty.hidden = Boolean(selected);

  if (selected) {
    // Avoid reloading the same image during unrelated gallery changes.
    if (collectionPreview.getAttribute("src") !== selected.url) {
      collectionPreview.src = selected.url;
    }
    collectionPreview.alt = `Six-color preview of ${selected.name}`;
    collectionName.textContent = selected.name;
    collectionDetails.textContent = `${selected.treatment} · ${selected.placement} · Browser preview`;
  } else {
    collectionPreview.removeAttribute("src");
    collectionPreview.alt = "";
    collectionName.textContent = "A space for something beautiful";
    collectionDetails.textContent = "Your collection is waiting to begin.";
  }

  for (const card of collectionGrid.children) {
    const isSelected = Number(card.dataset.artworkId) === selectedArtworkId;
    card.classList.toggle("selected", isSelected);
    card.querySelector(".artwork-pick").setAttribute("aria-pressed", String(isSelected));
    card.querySelector(".selection-label").textContent = isSelected ? "On the wall" : "In collection";
  }
}

function selectArtwork(id) {
  if (!signedIn || !collection.some((artwork) => artwork.id === id)) return;
  selectedArtworkId = id;

  if (slideshowTimer !== null) {
    nextChangeAt = Date.now() + intervalMilliseconds();
  }

  updateSelection();
  updateSlideshowStatus();
}

function requestDelete(id) {
  const artwork = collection.find((item) => item.id === id);
  if (!signedIn || !artwork) return;

  // Keep the artwork and confirmation stable while the dialog is open.
  pauseSlideshow();
  pendingDeleteId = id;
  deleteDescription.textContent = `“${artwork.name}” will be removed from this demo collection. Your original file will not be changed.`;
  deleteDialog.returnValue = "cancel";
  deleteDialog.showModal();
}

function collectionPageCount() {
  return Math.max(1, Math.ceil(collection.length / ARTWORKS_PER_PAGE));
}

function renderPagination() {
  const totalPages = collectionPageCount();
  collectionPagination.hidden = collection.length === 0;
  paginationSummary.textContent = `Page ${collectionPage} of ${totalPages} · ${collection.length} artwork${collection.length === 1 ? "" : "s"}`;
  const firstArtwork = (collectionPage - 1) * ARTWORKS_PER_PAGE + 1;
  const lastArtwork = Math.min(collectionPage * ARTWORKS_PER_PAGE, collection.length);
  paginationRange.textContent = collection.length === 0
    ? "No artworks yet."
    : `Showing ${firstArtwork}–${lastArtwork} · ${ARTWORKS_PER_PAGE} per page`;
  firstPageButton.disabled = previousPageButton.disabled = collectionPage === 1;
  lastPageButton.disabled = nextPageButton.disabled = collectionPage === totalPages;
  pageJumpInput.max = totalPages;
  pageJumpInput.value = collectionPage;
  pageJumpInput.setCustomValidity("");
  pageJumpInput.removeAttribute("aria-invalid");
  pageNumbers.replaceChildren();

  // A bounded window of shortcuts, even for collections with many pages.
  const shortcuts = new Set([1, totalPages]);
  const start = collectionPage <= 4 ? 2 : Math.max(2, collectionPage - 1);
  const end = collectionPage >= totalPages - 3
    ? totalPages - 1
    : Math.min(totalPages - 1, Math.max(5, collectionPage + 1));
  const windowStart = collectionPage >= totalPages - 3
    ? Math.max(2, Math.min(start, totalPages - 4))
    : start;

  for (let page = windowStart; page <= end; page++) shortcuts.add(page);

  function appendPageButton(page) {
    const button = document.createElement("button");
    button.type = "button";
    button.className = "button secondary page-number";
    button.textContent = page;
    button.dataset.page = page;
    button.setAttribute("aria-label", `Page ${page}`);
    button.setAttribute("aria-controls", "collection-grid");
    if (page === collectionPage) button.setAttribute("aria-current", "page");
    pageNumbers.append(button);
  }

  let previous = 0;
  for (const page of [...shortcuts].sort((a, b) => a - b)) {
    if (page - previous === 2) {
      appendPageButton(previous + 1);
    } else if (page - previous > 2) {
      const ellipsis = document.createElement("span");
      ellipsis.className = "pagination-ellipsis";
      ellipsis.textContent = "…";
      ellipsis.setAttribute("aria-hidden", "true");
      pageNumbers.append(ellipsis);
    }
    appendPageButton(page);
    previous = page;
  }
}

function goToCollectionPage(page) {
  if (!signedIn || !Number.isSafeInteger(page) || page < 1 || page > collectionPageCount()) return;
  if (page === collectionPage) return;

  const focusedControl = document.activeElement;
  const focusCurrentPage = pageNumbers.contains(focusedControl);
  collectionPage = page;
  renderCollection();

  // Number shortcuts are recreated; keep keyboard focus inside the navigation.
  if (focusCurrentPage || focusedControl?.disabled) {
    pageNumbers.querySelector('[aria-current="page"]').focus({ preventScroll: true });
  }
}

firstPageButton.addEventListener("click", () => goToCollectionPage(1));
previousPageButton.addEventListener("click", () => goToCollectionPage(collectionPage - 1));
nextPageButton.addEventListener("click", () => goToCollectionPage(collectionPage + 1));
lastPageButton.addEventListener("click", () => goToCollectionPage(collectionPageCount()));
pageNumbers.addEventListener("click", (event) => {
  const button = event.target.closest("button[data-page]");
  if (button) goToCollectionPage(Number(button.dataset.page));
});

pageJumpInput.addEventListener("input", () => {
  pageJumpInput.setCustomValidity("");
  pageJumpInput.removeAttribute("aria-invalid");
});

pageJumpForm.addEventListener("submit", (event) => {
  event.preventDefault();
  if (!signedIn) return;
  const page = pageJumpInput.valueAsNumber;
  pageJumpInput.setCustomValidity("");

  if (!Number.isSafeInteger(page) || page < 1 || page > collectionPageCount()) {
    pageJumpInput.setCustomValidity(`Enter a whole page number from 1 to ${collectionPageCount()}.`);
    pageJumpInput.setAttribute("aria-invalid", "true");
    pageJumpInput.reportValidity();
    return;
  }

  pageJumpInput.removeAttribute("aria-invalid");
  goToCollectionPage(page);
  // Normalize valid alternative number representations, including leading zeros.
  pageJumpInput.value = collectionPage;
});

function renderCollection() {
  collectionPage = Math.max(1, Math.min(collectionPage, collectionPageCount()));
  collectionCount.textContent = `${collection.length} artwork${collection.length === 1 ? "" : "s"}`;
  collectionListEmpty.hidden = collection.length > 0;
  collectionGrid.replaceChildren();

  const start = (collectionPage - 1) * ARTWORKS_PER_PAGE;
  for (const artwork of collection.slice(start, start + ARTWORKS_PER_PAGE)) {
    const card = document.createElement("article");
    card.className = "collection-item";
    card.dataset.artworkId = artwork.id;

    const selectButton = document.createElement("button");
    selectButton.type = "button";
    selectButton.className = "artwork-pick";
    selectButton.addEventListener("click", () => selectArtwork(artwork.id));

    const image = document.createElement("img");
    image.src = artwork.url;
    image.alt = "";
    image.loading = "lazy";
    image.width = WIDTH;
    image.height = HEIGHT;

    const name = document.createElement("span");
    name.textContent = artwork.name;
    name.title = artwork.name;
    selectButton.append(image, name);

    const footer = document.createElement("div");
    footer.className = "collection-item-footer";

    const selectionLabel = document.createElement("small");
    selectionLabel.className = "selection-label";

    const deleteButton = document.createElement("button");
    deleteButton.type = "button";
    deleteButton.className = "delete-artwork";
    deleteButton.textContent = "Remove";
    deleteButton.setAttribute("aria-label", `Remove ${artwork.name}`);
    deleteButton.addEventListener("click", () => requestDelete(artwork.id));

    footer.append(selectionLabel, deleteButton);
    card.append(selectButton, footer);
    collectionGrid.append(card);
  }

  slideshowButton.disabled = collection.length < 2;
  if (collection.length < 2) pauseSlideshow();
  renderPagination();
  updateSelection();
  syncEditorControls();
}

deleteDialog.addEventListener("close", () => {
  const id = pendingDeleteId;
  pendingDeleteId = null;
  if (!signedIn || deleteDialog.returnValue !== "delete") return;

  const index = collection.findIndex((artwork) => artwork.id === id);
  if (index === -1) return;

  const [removed] = collection.splice(index, 1);
  if (selectedArtworkId === id) {
    selectedArtworkId = collection[Math.min(index, collection.length - 1)]?.id ?? null;
  }

  renderCollection();
  URL.revokeObjectURL(removed.url);
  galleryFeedback.textContent = `“${removed.name}” removed from the demo collection.`;

  // The dialog's original trigger was replaced when rebuilding the grid.
  const nextButton = collectionGrid.querySelector(".artwork-pick");
  (nextButton || chooseButton).focus({ preventScroll: true });
});

addButton.addEventListener("click", () => {
  if (!signedIn || !previewReady || loadingImage || addingArtwork) return;

  addingArtwork = true;
  syncEditorControls();
  const name = sourceName;
  const currentSession = sessionVersion;
  const treatment = ditherSelect.value === "none" ? "Nearest color" : "Floyd–Steinberg";
  const placement = fitSelect.value === "cover" ? "Cropped to fill" : "Fit to frame";

  canvas.toBlob((blob) => {
    addingArtwork = false;
    syncEditorControls();
    if (!signedIn || currentSession !== sessionVersion) return;
    if (!blob) {
      setFeedback("Could not save this artwork. Please try again.", true);
      return;
    }

    const artwork = {
      id: nextArtworkId++,
      name,
      treatment,
      placement,
      url: URL.createObjectURL(blob),
    };

    collection.push(artwork);
    if (selectedArtworkId === null) selectedArtworkId = artwork.id;
    // Reveal the new snapshot without changing the exhibited artwork or slideshow.
    collectionPage = collectionPageCount();
    renderCollection();
    setFeedback(`“${name}” added to your gallery. Prepare another piece or explore your collection above.`);
    galleryFeedback.textContent = `“${name}” added to the demo collection.`;
  }, "image/png");
});

slideshowButton.addEventListener("click", () => {
  if (!signedIn || collection.length < 2) return;
  if (slideshowTimer !== null) {
    pauseSlideshow();
    return;
  }

  nextChangeAt = Date.now() + intervalMilliseconds();
  slideshowButton.textContent = "Pause slideshow";
  slideshowButton.setAttribute("aria-pressed", "true");

  slideshowTimer = window.setInterval(() => {
    if (Date.now() >= nextChangeAt) {
      const index = collection.findIndex((artwork) => artwork.id === selectedArtworkId);
      selectedArtworkId = collection[(index + 1) % collection.length].id;

      // No rapid catch-up after a suspended or backgrounded tab.
      nextChangeAt = Date.now() + intervalMilliseconds();
      updateSelection();
    }

    updateSlideshowStatus();
  }, 1000);

  updateSlideshowStatus();
});

slideshowInterval.addEventListener("change", () => {
  if (slideshowTimer !== null) nextChangeAt = Date.now() + intervalMilliseconds();
  updateSlideshowStatus();
});

renderCollection();
// Do not allow the form's default submission if the script fails to initialize.
loginForm.inert = false;
