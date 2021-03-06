/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *                                                                           *
 *  Copyright (C) 2020-2020 Chuan Ji                                         *
 *                                                                           *
 *  Licensed under the Apache License, Version 2.0 (the "License");          *
 *  you may not use this file except in compliance with the License.         *
 *  You may obtain a copy of the License at                                  *
 *                                                                           *
 *   http://www.apache.org/licenses/LICENSE-2.0                              *
 *                                                                           *
 *  Unless required by applicable law or agreed to in writing, software      *
 *  distributed under the License is distributed on an "AS IS" BASIS,        *
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. *
 *  See the License for the specific language governing permissions and      *
 *  limitations under the License.                                           *
 *                                                                           *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

// This file defines FitzDocument, an implementation of the Document
// abstraction using Fitz.

#include "fitz_document.hpp"

#include <cassert>

#include "multithreading.hpp"
#include "string_utils.hpp"

FitzDocument* FitzDocument::Open(
    const std::string& path, const std::string* password) {
  fz_context* fz_ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
  fz_register_document_handlers(fz_ctx);
  // Disable warning messages in the console.
  fz_set_warning_callback(
      fz_ctx, [](void* user, const char* message) {}, nullptr);

  fz_document* fz_doc = nullptr;
  fz_try(fz_ctx) {
    fz_doc = fz_open_document(fz_ctx, path.c_str());
    if ((fz_doc == nullptr) || (!fz_count_pages(fz_ctx, fz_doc))) {
      fz_throw(
          fz_ctx, FZ_ERROR_GENERIC,
          const_cast<char*>("Cannot open document \"%s\""), path.c_str());
    }
    if (fz_needs_password(fz_ctx, fz_doc)) {
      if (password == nullptr) {
        fz_throw(
            fz_ctx, FZ_ERROR_GENERIC,
            const_cast<char*>(
                "Document \"%s\" is password protected.\n"
                "Please provide the password with \"-P <password>\"."),
            path.c_str());
      }
      if (!fz_authenticate_password(fz_ctx, fz_doc, password->c_str())) {
        fz_throw(
            fz_ctx, FZ_ERROR_GENERIC,
            const_cast<char*>("Incorrect password for document \"%s\"."),
            path.c_str());
      }
    }
  }
  fz_catch(fz_ctx) {
    if (fz_doc != nullptr) {
      fz_drop_document(fz_ctx, fz_doc);
    }
    fz_drop_context(fz_ctx);
    return nullptr;
  }

  return new FitzDocument(fz_ctx, fz_doc);
}

FitzDocument::FitzDocument(fz_context* fz_ctx, fz_document* fz_doc)
    : _fz_ctx(fz_ctx), _fz_doc(fz_doc) {
  assert(_fz_ctx != nullptr);
  assert(_fz_doc != nullptr);
}

FitzDocument::~FitzDocument() {
  std::lock_guard<std::recursive_mutex> lock(_fz_mutex);
  fz_drop_document(_fz_ctx, _fz_doc);
  fz_drop_context(_fz_ctx);
}

int FitzDocument::GetNumPages() {
  std::lock_guard<std::recursive_mutex> lock(_fz_mutex);
  return fz_count_pages(_fz_ctx, _fz_doc);
}

const Document::PageSize FitzDocument::GetPageSize(
    int page, float zoom, int rotation) {
  std::lock_guard<std::recursive_mutex> lock(_fz_mutex);
  assert((page >= 0) && (page < GetNumPages()));
  FitzPageScopedPtr page_ptr(_fz_ctx, fz_load_page(_fz_ctx, _fz_doc, page));
  const fz_irect& bbox = GetPageBoundingBox(
      _fz_ctx, page_ptr.get(), ComputeTransformMatrix(zoom, rotation));
  return PageSize(bbox.x1 - bbox.x0, bbox.y1 - bbox.y0);
}

void FitzDocument::Render(
    Document::PixelWriter* pw, int page, float zoom, int rotation) {
  std::lock_guard<std::recursive_mutex> lock(_fz_mutex);
  assert((page >= 0) && (page < GetNumPages()));

  // 1. Init MuPDF structures.
  const fz_matrix& m = ComputeTransformMatrix(zoom, rotation);
  FitzPageScopedPtr page_ptr(_fz_ctx, fz_load_page(_fz_ctx, _fz_doc, page));
  const fz_irect& bbox = GetPageBoundingBox(_fz_ctx, page_ptr.get(), m);
  FitzPixmapScopedPtr pixmap_ptr(
      _fz_ctx, fz_new_pixmap_with_bbox(
                   _fz_ctx, fz_device_rgb(_fz_ctx), bbox, nullptr, 1));
  FitzDeviceScopedPtr dev_ptr(
      _fz_ctx, fz_new_draw_device(_fz_ctx, fz_identity, pixmap_ptr.get()));

  // 2. Render page.
  fz_clear_pixmap_with_value(_fz_ctx, pixmap_ptr.get(), 0xff);
  fz_run_page(_fz_ctx, page_ptr.get(), dev_ptr.get(), m, nullptr);

  // 3. Write pixmap to buffer. The page is vertically divided into n equal
  // stripes, each copied to pw by one thread.
  assert(fz_pixmap_components(_fz_ctx, pixmap_ptr.get()) == 4);
  uint8_t* buffer =
      reinterpret_cast<uint8_t*>(fz_pixmap_samples(_fz_ctx, pixmap_ptr.get()));
  const int num_cols = fz_pixmap_width(_fz_ctx, pixmap_ptr.get());
  const int num_rows = fz_pixmap_height(_fz_ctx, pixmap_ptr.get());
  ExecuteInParallel([=](int num_threads, int i) {
    const int num_rows_per_thread = num_rows / num_threads;
    const int y_begin = i * num_rows_per_thread;
    const int y_end =
        (i == num_threads - 1) ? num_rows : (i + 1) * num_rows_per_thread;
    uint8_t* p = buffer + y_begin * num_cols * 4;
    for (int y = y_begin; y < y_end; ++y) {
      for (int x = 0; x < num_cols; ++x) {
        pw->Write(x, y, p[0], p[1], p[2]);
        p += 4;
      }
    }
  });

  // 4. Clean up.
  fz_close_device(_fz_ctx, dev_ptr.get());
}

const Document::OutlineItem* FitzDocument::GetOutline() {
  std::lock_guard<std::recursive_mutex> lock(_fz_mutex);
  FitzOutlineScopedPtr outline_ptr(_fz_ctx, fz_load_outline(_fz_ctx, _fz_doc));
  if (outline_ptr.get() == nullptr) {
    return nullptr;
  }
  FitzOutlineItem* root = FitzOutlineItem::Build(_fz_ctx, outline_ptr.get());
  return root;
}

int FitzDocument::Lookup(const OutlineItem* item) {
  return (dynamic_cast<const FitzOutlineItem*>(item))->GetDestPage();
}

std::string FitzDocument::GetPageText(int page, int line_sep) {
  std::lock_guard<std::recursive_mutex> lock(_fz_mutex);
  FitzPageScopedPtr page_ptr(_fz_ctx, fz_load_page(_fz_ctx, _fz_doc, page));
  return ::GetPageText(_fz_ctx, page_ptr.get(), line_sep);
}

std::vector<Document::SearchHit> FitzDocument::SearchOnPage(
    const std::string& search_string, int page, int context_length) {
  const size_t margin =
      context_length > static_cast<int>(search_string.length())
          ? (context_length - search_string.length() + 1) / 2
          : 0;

  const std::string page_text = GetPageText(page, ' ');
  std::vector<SearchHit> search_hits;
  for (size_t pos = 0;; ++pos) {
    if ((pos = CaseInsensitiveSearch(page_text, search_string, pos)) ==
        std::string::npos) {
      break;
    }
    const size_t context_start_pos = pos >= margin ? pos - margin : 0;
    search_hits.emplace_back(
        page, page_text.substr(context_start_pos, context_length),
        pos - context_start_pos);
  }
  return search_hits;
}

