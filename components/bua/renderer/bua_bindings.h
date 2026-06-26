// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_BUA_RENDERER_BUA_BINDINGS_H_
#define COMPONENTS_BUA_RENDERER_BUA_BINDINGS_H_

namespace content {
class RenderFrame;
}

namespace bua {

class BuaBindings {
 public:
  BuaBindings() = delete;

  static void Install(content::RenderFrame* render_frame);
};

}  // namespace bua

#endif  // COMPONENTS_BUA_RENDERER_BUA_BINDINGS_H_
