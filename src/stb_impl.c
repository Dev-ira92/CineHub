/* stb_impl.c - jedina translacijska jedinica koja instancira stb_image.
 * Ostali fajlovi ukljucuju samo deklaracije iz stb_image.h.
 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_ONLY_TGA
#define STBI_NO_STDIO       /* citamo iskljucivo iz memorije */
#define STBI_NO_HDR
#define STBI_NO_LINEAR

#include "stb_image.h"
