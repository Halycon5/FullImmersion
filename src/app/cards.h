// Small text cards for the room (agents' name tags, notes), drawn with GDI into textures, kept while in use.
// And encode_png, for the pictures agents ask for.
#pragma once
#include "common/desktop.h"
#include <map>
#include <string>

namespace fi {

class Cards {
 public:
  explicit Cards(ID3D11Device* device) : device_(device) {}
  // A card with a bold title over wrapped body text, an accent stripe down its left side. aspect = height/width.
  ID3D11ShaderResourceView* get(const std::wstring& title, const std::wstring& body, uint32_t accent_rgb,
                                float& aspect);
  void sweep();  // drop cards not asked for since the last sweep

 private:
  struct Card {
    winrt::com_ptr<ID3D11Texture2D> tex;
    winrt::com_ptr<ID3D11ShaderResourceView> srv;
    float aspect = 0.25f;
    bool used = true;
  };
  ID3D11Device* device_;
  std::map<std::wstring, Card> cards_;
};

std::string encode_png(const std::vector<uint32_t>& bgra, UINT w, UINT h);  // empty on failure

}  // namespace fi
