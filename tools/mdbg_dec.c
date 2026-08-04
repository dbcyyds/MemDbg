/* 极简 XOR 解密器：读加密文件 → 写出明文（gzip 流）到 stdout
 * 用法: mdbg_dec <enc> <ka_hex> <kb_hex> <kc_hex>
 * 算法与 dbc awk 一致，便于大文件快速解密。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int unhex(const char *hx, uint8_t *out, int maxb) {
  int n = 0;
  for (const char *p = hx; p[0] && p[1] && n < maxb; p += 2) {
    int a = hexv(p[0]), b = hexv(p[1]);
    if (a < 0 || b < 0) return -1;
    out[n++] = (uint8_t)((a << 4) | b);
  }
  return n;
}

int main(int argc, char **argv) {
  if (argc < 5) {
    fprintf(stderr, "usage: mdbg_dec enc ka kb kc\n");
    return 2;
  }
  uint8_t ka[64], kb[64], kc[64];
  int la = unhex(argv[2], ka, 64);
  int lb = unhex(argv[3], kb, 64);
  int lc = unhex(argv[4], kc, 64);
  if (la <= 0 || lb <= 0 || lc <= 0) {
    fprintf(stderr, "bad key\n");
    return 3;
  }
  FILE *f = fopen(argv[1], "rb");
  if (!f) {
    perror("open");
    return 4;
  }
  uint8_t buf[1 << 16];
  size_t off = 0;
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    for (size_t i = 0; i < n; i++) {
      size_t p = off + i;
      uint8_t b = buf[i];
      b ^= (uint8_t)(p & 255) ^ (uint8_t)((p / 256) & 255) ^ kc[p % (size_t)lc];
      b ^= kb[p % (size_t)lb];
      b ^= ka[p % (size_t)la];
      buf[i] = b;
    }
    if (fwrite(buf, 1, n, stdout) != n) {
      fclose(f);
      return 5;
    }
    off += n;
  }
  fclose(f);
  return 0;
}
