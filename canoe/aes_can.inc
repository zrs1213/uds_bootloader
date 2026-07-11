/* =============================================================================
 * aes_can.inc  —— CANoe CAPL 版 AES-128-ECB + UDS $27 Seed&Key 派生
 * -----------------------------------------------------------------------------
 * 本文件是 src/security.c 的逐行移植，保证 CANoe 侧与 C 协议栈行为完全一致。
 *
 * 重要：本 .inc 只含“函数”，不含 variables 块！
 *   SBOX / RCON / g_master / g_rng_state 必须在引用它的主 .can 文件的
 *   variables {} 中声明（tester 与 ecu 均已声明），否则编译报 Unknown symbol。
 *
 * CAPL 语法限制（已遵守）：
 *   - 数组参数只能写 byte x[]（不能写 byte x[16]）
 *   - 不支持 const 全局、指针、引用
 * ========================================================================== */

/* GF(2^8) 乘法 */
byte gmul(byte a, byte b) {
  byte p; int i; byte hi;
  p = 0;
  for (i = 0; i < 8; i++) {
    if (b & 1) p ^= a;
    hi = (byte)(a & 0x80);
    a = (byte)(a << 1);
    if (hi) a ^= 0x1b;
    b = (byte)(b >> 1);
  }
  return p;
}

void KeyExpansion(byte key[], byte w[]) {
  int i, j; byte tmp[4]; byte t;
  for (i = 0; i < 16; i++) w[i] = key[i];
  for (i = 16; i < 176; i += 4) {
    for (j = 0; j < 4; j++) tmp[j] = w[i - 4 + j];
    if (i % 16 == 0) {
      t = tmp[0];
      tmp[0] = (byte)(SBOX[tmp[1]] ^ RCON[(i / 16) - 1]);
      tmp[1] = SBOX[tmp[2]];
      tmp[2] = SBOX[tmp[3]];
      tmp[3] = SBOX[t];
    }
    for (j = 0; j < 4; j++) w[i + j] = (byte)(w[i - 16 + j] ^ tmp[j]);
  }
}

void AddRoundKey(byte s[], byte w[], int off) {
  int i;
  for (i = 0; i < 16; i++) s[i] = (byte)(s[i] ^ w[off + i]);
}

void SubBytes(byte s[]) {
  int i;
  for (i = 0; i < 16; i++) s[i] = SBOX[s[i]];
}

void ShiftRows(byte s[]) {
  byte t;
  t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
  t = s[2]; s[2] = s[10]; s[10] = t; t = s[6]; s[6] = s[14]; s[14] = t;
  t = s[3]; s[3] = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = t;
}

void MixColumns(byte s[]) {
  int c; int i; byte a0, a1, a2, a3;
  for (c = 0; c < 4; c++) {
    i = 4 * c;
    a0 = s[i]; a1 = s[i+1]; a2 = s[i+2]; a3 = s[i+3];
    s[i]   = (byte)(gmul(a0,2) ^ gmul(a1,3) ^ a2 ^ a3);
    s[i+1] = (byte)(a0 ^ gmul(a1,2) ^ gmul(a2,3) ^ a3);
    s[i+2] = (byte)(a0 ^ a1 ^ gmul(a2,2) ^ gmul(a3,3));
    s[i+3] = (byte)(gmul(a0,3) ^ a1 ^ a2 ^ gmul(a3,2));
  }
}

void Cipher(byte in[], byte out[], byte w[]) {
  byte state[16]; int round, i;
  for (i = 0; i < 16; i++) state[i] = in[i];
  AddRoundKey(state, w, 0);
  for (round = 1; round < 10; round++) {
    SubBytes(state);
    ShiftRows(state);
    MixColumns(state);
    AddRoundKey(state, w, round * 16);
  }
  SubBytes(state);
  ShiftRows(state);
  AddRoundKey(state, w, 160);
  for (i = 0; i < 16; i++) out[i] = state[i];
}

void AES128_ECB_Encrypt(byte key[], byte in[], byte out[]) {
  byte w[176];
  KeyExpansion(key, w);
  Cipher(in, out, w);
}

/* ---------------- Seed&Key 派生（对应 security.c） ---------------- */

void SEC_Init(byte key[]) {
  int i;
  for (i = 0; i < 16; i++) g_master[i] = key[i];
}

void SEC_RngSeed(dword seed) {
  g_rng_state = seed ? seed : 0x12345678;
}

dword rng_next(void) {
  dword x;
  x = g_rng_state;
  x = (dword)(x ^ (x << 13));
  x = (dword)(x ^ (x >> 17));
  x = (dword)(x ^ (x << 5));
  g_rng_state = x;
  return x;
}

void SEC_GenSeed(byte seed[]) {
  int i; dword v;
  for (i = 0; i < 16; i += 4) {
    v = rng_next();
    seed[i]   = (byte)(v & 0xFF);
    seed[i+1] = (byte)((v >> 8) & 0xFF);
    seed[i+2] = (byte)((v >> 16) & 0xFF);
    seed[i+3] = (byte)((v >> 24) & 0xFF);
  }
}

void SEC_ComputeKey(byte seed[], byte key[]) {
  AES128_ECB_Encrypt(g_master, seed, key);
}
