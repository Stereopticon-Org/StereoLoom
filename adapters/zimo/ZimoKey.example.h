// ZimoKey.example.h -- copy to ZimoKey.private.h (never committed) and put Stereo
// Loom's Zondision app key in it to build ZimoAdapter.dll with that key. Without
// ZimoKey.private.h the adapter is built with Zondision's evaluation key, which
// expires. Keys come from Zondision's developer portal (developer.zondision.com).
#pragma once
#define ZIMO_APP_KEY "your-app-key-here"
