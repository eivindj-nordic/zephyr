/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <ztest.h>
#include <sys/cbprintf.h>
#include <linker/utils.h>

#define CBPRINTF_DEBUG 1

#ifndef CBPRINTF_PACKAGE_ALIGN_OFFSET
#define CBPRINTF_PACKAGE_ALIGN_OFFSET 0
#endif

#define ALIGN_OFFSET (sizeof(void *) * CBPRINTF_PACKAGE_ALIGN_OFFSET)

struct out_buffer {
	char *buf;
	size_t idx;
	size_t size;
};

static int out(int c, void *dest)
{
	int rv = EOF;
	struct out_buffer *buf = (struct out_buffer *)dest;

	if (buf->idx < buf->size) {
		buf->buf[buf->idx++] = (char)(unsigned char)c;
		rv = (int)(unsigned char)c;
	}
	return rv;
}

static char static_buf[512];
static char runtime_buf[512];
static char compare_buf[128];

static void dump(const char *desc, uint8_t *package, size_t len)
{
	printk("%s package %p:\n", desc, package);
	for (size_t i = 0; i < len; i++) {
		printk("%02x ", package[i]);
	}
	printk("\n");
}

static void unpack(const char *desc, struct out_buffer *buf,
	    uint8_t *package, size_t len)
{
	cbpprintf((cbprintf_cb)out, buf, package);
	buf->buf[buf->idx] = 0;
	zassert_equal(strcmp(buf->buf, compare_buf), 0,
		      "Strings differ\nexp: |%s|\ngot: |%s|\n",
		      compare_buf, buf->buf);
}

#define TEST_PACKAGING(flags, fmt, ...) do { \
	int must_runtime = CBPRINTF_MUST_RUNTIME_PACKAGE(flags, fmt, __VA_ARGS__); \
	zassert_equal(must_runtime, !Z_C_GENERIC, NULL); \
	snprintfcb(compare_buf, sizeof(compare_buf), fmt, __VA_ARGS__); \
	printk("-----------------------------------------\n"); \
	printk("%s\n", compare_buf); \
	uint8_t *pkg; \
	struct out_buffer rt_buf = { \
		.buf = runtime_buf, .idx = 0, .size = sizeof(runtime_buf) \
	}; \
	int rc = cbprintf_package(NULL, ALIGN_OFFSET, 0, fmt, __VA_ARGS__); \
	zassert_true(rc > 0, "cbprintf_package() returned %d", rc); \
	int len = rc; \
	/* Aligned so the package is similar to the static one. */ \
	uint8_t __aligned(CBPRINTF_PACKAGE_ALIGNMENT) \
			rt_package[len + ALIGN_OFFSET]; \
	memset(rt_package, 0, len + ALIGN_OFFSET); \
	pkg = &rt_package[ALIGN_OFFSET]; \
	rc = cbprintf_package(pkg, len, 0, fmt, __VA_ARGS__); \
	zassert_equal(rc, len, "cbprintf_package() returned %d, expected %d", \
		      rc, len); \
	dump("runtime", pkg, len); \
	unpack("runtime", &rt_buf, pkg, len); \
	struct out_buffer st_buf = { \
		.buf = static_buf, .idx = 0, .size = sizeof(static_buf) \
	}; \
	CBPRINTF_STATIC_PACKAGE(NULL, 0, len, ALIGN_OFFSET, flags, fmt, __VA_ARGS__); \
	zassert_true(len > 0, "CBPRINTF_STATIC_PACKAGE() returned %d", len); \
	uint8_t __aligned(CBPRINTF_PACKAGE_ALIGNMENT) \
		package[len + ALIGN_OFFSET];\
	int outlen; \
	pkg = &package[ALIGN_OFFSET]; \
	CBPRINTF_STATIC_PACKAGE(pkg, len, outlen, ALIGN_OFFSET, flags, fmt, __VA_ARGS__);\
	zassert_equal(len, outlen, NULL); \
	dump("static", pkg, len); \
	unpack("static", &st_buf, pkg, len); \
} while (0)

void test_cbprintf_package(void)
{
	volatile signed char sc = -11;
	int i = 100;
	char c = 'a';
	static const short s = -300;
	long li = -1111111111;
	long long lli = 0x1122334455667788;
	unsigned char uc = 100;
	unsigned int ui = 0x12345;
	unsigned short us = 0x1234;
	unsigned long ul = 0xaabbaabb;
	unsigned long long ull = 0xaabbaabbaabb;
	void *vp = NULL;
	static const char *str = "test";
	char *pstr = (char *)str;
	int rv;

	/* tests to exercize different element alignments */
	TEST_PACKAGING(0, "test long %x %lx %x", 0xb1b2b3b4, li, 0xe4e3e2e1);
	TEST_PACKAGING(0, "test long long %x %llx %x", 0xb1b2b3b4, lli, 0xe4e3e2e1);

	/* tests with varied elements */
	TEST_PACKAGING(0, "test %d %hd %hhd", i, s, sc);
	TEST_PACKAGING(0, "test %ld %llx %hhu %hu %u", li, lli, uc, us, ui);
	TEST_PACKAGING(0, "test %lu %llu", ul, ull);
	TEST_PACKAGING(0, "test %c %p", c, vp);

	/* Runtime packaging is still possible when const strings are used. */
	TEST_PACKAGING(CBPRINTF_PACKAGE_CONST_CHAR_RO,
			"test %s %s", str, (const char *)pstr);

	/* When flag is set but argument is char * runtime packaging must be used. */
	rv = CBPRINTF_MUST_RUNTIME_PACKAGE(CBPRINTF_PACKAGE_CONST_CHAR_RO,
					   "test %s %s", str, pstr);
	zassert_true(rv != 0, "Unexpected value %d", rv);

	/* When const char * are used but flag is not used then runtime packaging must be used. */
	rv = CBPRINTF_MUST_RUNTIME_PACKAGE(0, "test %s %s", str, (const char *)pstr);
	zassert_true(rv != 0, "Unexpected value %d", rv);

	rv = CBPRINTF_MUST_RUNTIME_PACKAGE(CBPRINTF_PACKAGE_CONST_CHAR_RO,
					   "test %s", (char *)str);
	zassert_true(rv != 0, "Unexpected value %d", rv);

	if (IS_ENABLED(CONFIG_CBPRINTF_FP_SUPPORT)) {
		float f = -1.234f;
		double d = 1.2333;

		TEST_PACKAGING(0, "test double %x %f %x", 0xb1b2b3b4, d, 0xe4e3e2e1);
		TEST_PACKAGING(0, "test %f %a", (double)f, d);
#if CONFIG_CBPRINTF_PACKAGE_LONGDOUBLE && !(defined(CONFIG_RISCV) && !defined(CONFIG_64BIT))
		/* Excluding riscv32 which does not handle long double correctly. */
		long double ld = 1.2333;

		TEST_PACKAGING(0, "test %Lf", ld);
#endif
	}
}

void test_cbprintf_rw_str_indexes(void)
{
	int len0, len1, len2;
	static const char *test_str = "test %d %s";
	static const char *test_str1 = "lorem ipsum";
	uint8_t str_idx;
	char *addr;

	len0 = cbprintf_package(NULL, 0, 0, test_str, 100, test_str1);
	if (len0 > (int)(4 * sizeof(void *))) {
		TC_PRINT("Skipping test, platform does not detect RO strings.\n");
		ztest_test_skip();
	}

	zassert_true(len0 > 0, NULL);
	len1 = cbprintf_package(NULL, 0, CBPRINTF_PACKAGE_ADD_STRING_IDXS,
				test_str, 100, test_str1);
	zassert_true(len1 > 0, NULL);

	CBPRINTF_STATIC_PACKAGE(NULL, 0, len2, 0,
				CBPRINTF_PACKAGE_ADD_STRING_IDXS,
				test_str, 100, test_str1);
	zassert_true(len2 > 0, NULL);

	/* package with string indexes will contain two more bytes holding indexes
	 * of string parameter locations.
	 */
	zassert_equal(len0 + 2, len1, NULL);
	zassert_equal(len0 + 2, len2, NULL);

	uint8_t __aligned(CBPRINTF_PACKAGE_ALIGNMENT) package0[len0];
	uint8_t __aligned(CBPRINTF_PACKAGE_ALIGNMENT) package1[len1];
	uint8_t __aligned(CBPRINTF_PACKAGE_ALIGNMENT) package2[len2];

	len0 = cbprintf_package(package0, sizeof(package0), 0,
				test_str, 100, test_str1);

	len1 = cbprintf_package(package1, sizeof(package1) - 1,
				CBPRINTF_PACKAGE_ADD_STRING_IDXS,
				test_str, 100, test_str1);
	zassert_equal(-ENOSPC, len1, NULL);

	CBPRINTF_STATIC_PACKAGE(package2, sizeof(package2) - 1, len2, 0,
				CBPRINTF_PACKAGE_ADD_STRING_IDXS,
				test_str, 100, test_str1);
	zassert_equal(-ENOSPC, len2, NULL);

	len1 = cbprintf_package(package1, sizeof(package1),
				CBPRINTF_PACKAGE_ADD_STRING_IDXS,
				test_str, 100, test_str1);
	zassert_equal(len0 + 2, len1, NULL);

	CBPRINTF_STATIC_PACKAGE(package2, sizeof(package2), len2, 0,
				CBPRINTF_PACKAGE_ADD_STRING_IDXS,
				test_str, 100, test_str1);
	zassert_equal(len0 + 2, len2, NULL);

	struct z_cbprintf_desc *desc0 = (struct z_cbprintf_desc *)package0;
	struct z_cbprintf_desc *desc1 = (struct z_cbprintf_desc *)package1;
	struct z_cbprintf_desc *desc2 = (struct z_cbprintf_desc *)package2;

	/* Compare descriptor content. Second package has one ro string index. */
	zassert_equal(desc0->ro_str_cnt, 0, NULL);
	zassert_equal(desc1->ro_str_cnt, 2, NULL);
	zassert_equal(desc2->ro_str_cnt, 2, NULL);

	int *p = (int *)package1;

	str_idx = package1[len0];
	addr = *(char **)&p[str_idx];
	zassert_equal(addr, test_str, NULL);

	str_idx = package2[len0];
	addr = *(char **)&p[str_idx];
	zassert_equal(addr, test_str, NULL);

	str_idx = package1[len0 + 1];
	addr = *(char **)&p[str_idx];
	zassert_equal(addr, test_str1, NULL);

	str_idx = package2[len0 + 1];
	addr = *(char **)&p[str_idx];
	zassert_equal(addr, test_str1, NULL);
}

static void test_cbprintf_fsc_package(void)
{
	int len;
	static const char *test_str = "test %d %s";
	static const char *test_str1 = "lorem ipsum";
	char *addr;
	int fsc_len;

	len = cbprintf_package(NULL, 0, CBPRINTF_PACKAGE_ADD_STRING_IDXS,
			       test_str, 100, test_str1);
	if (len > (int)(4 * sizeof(void *) + 2)) {
		TC_PRINT("Skipping test, platform does not detect RO strings.\n");
		ztest_test_skip();
	}

	uint8_t __aligned(CBPRINTF_PACKAGE_ALIGNMENT) package[len];

	len = cbprintf_package(package, sizeof(package),
				CBPRINTF_PACKAGE_ADD_STRING_IDXS,
				test_str, 100, test_str1);

	struct z_cbprintf_desc *desc = (struct z_cbprintf_desc *)package;

	zassert_equal(desc->ro_str_cnt, 2, NULL);
	zassert_equal(desc->str_cnt, 0, NULL);

	/* Get length of fsc package. */
	fsc_len = cbprintf_fsc_package(package, len, NULL, 0);

	int exp_len = len + (int)strlen(test_str) + 1 + (int)strlen(test_str1) + 1;

	zassert_equal(exp_len, fsc_len, NULL);

	uint8_t __aligned(CBPRINTF_PACKAGE_ALIGNMENT) fsc_package[fsc_len];

	fsc_len = cbprintf_fsc_package(package, len, fsc_package, sizeof(fsc_package) - 1);
	zassert_equal(fsc_len, -ENOSPC, NULL);

	fsc_len = cbprintf_fsc_package(package, len, fsc_package, sizeof(fsc_package));
	zassert_equal((int)sizeof(fsc_package), fsc_len, NULL);

	/* New package has no RO string locations, only copied one. */
	desc = (struct z_cbprintf_desc *)fsc_package;
	zassert_equal(desc->ro_str_cnt, 0, NULL);
	zassert_equal(desc->str_cnt, 2, NULL);

	/* Get pointer to the first string in the package. */
	addr = (char *)&fsc_package[desc->len * sizeof(int) + 1];

	zassert_equal(strcmp(test_str, addr), 0, NULL);

	/* Get address of the second string. */
	addr += strlen(addr) + 2;
	zassert_equal(strcmp(test_str1, addr), 0, NULL);
}

static void check_package(void *package, size_t len, const char *exp_str)
{
	char out_str[128];

	strcpy(compare_buf, exp_str);

	struct out_buffer out_buf = {
		.buf = out_str,
		.idx = 0,
		.size = sizeof(out_str)
	};

	unpack(NULL, &out_buf, (uint8_t *)package, len);
}

static void test_cbprintf_ro_loc(void)
{
	static const char *test_str = "test %d";
	uint32_t flags = CBPRINTF_PACKAGE_ADD_RO_STR_POS;

#define TEST_FMT test_str, 100
	char exp_str[256];

	snprintfcb(exp_str, sizeof(exp_str), TEST_FMT);

	int len = cbprintf_package(NULL, 0, flags, TEST_FMT);

	int slen;

	CBPRINTF_STATIC_PACKAGE(NULL, 0, slen, ALIGN_OFFSET, flags, TEST_FMT);

	zassert_true(len > 0, NULL);
	zassert_equal(len, slen, "Runtime length: %d, static length: %d", len, slen);

	uint8_t __aligned(CBPRINTF_PACKAGE_ALIGNMENT) package[len];
	uint8_t __aligned(CBPRINTF_PACKAGE_ALIGNMENT) spackage[slen];

	len = cbprintf_package(package, sizeof(package), flags, TEST_FMT);
	CBPRINTF_STATIC_PACKAGE(spackage, sizeof(spackage), slen, ALIGN_OFFSET, flags, TEST_FMT);

	zassert_true(len > 0, NULL);
	zassert_equal(len, slen, "Runtime length: %d, static length: %d", len, slen);

	zassert_equal(memcmp(package, spackage, len), 0, NULL);

	uint8_t *hdr = package;

	/* Check that only read-only string location array size is non zero */
	zassert_equal(hdr[1], 0, NULL);
	zassert_equal(hdr[2], 1, NULL);
	zassert_equal(hdr[3], 0, NULL);

	int clen;

	/* Calculate size needed for package with appended read-only strings. */
	clen = cbprintf_package_copy(package, sizeof(package), NULL, 0,
				     CBPRINTF_PACKAGE_COPY_RO_STR, NULL, 0);

	/* Length will be increased by string length + null terminator. */
	zassert_equal(clen, len + (int)strlen(test_str) + 1, NULL);

	uint8_t __aligned(CBPRINTF_PACKAGE_ALIGNMENT) cpackage[clen];

	int clen2 = cbprintf_package_copy(package, sizeof(package), cpackage, sizeof(cpackage),
				     CBPRINTF_PACKAGE_COPY_RO_STR, NULL, 0);

	zassert_equal(clen, clen2, NULL);

	/* Length will be increased by string length + null terminator. */
	zassert_equal(clen, len + (int)strlen(test_str) + 1, NULL);

	uint8_t *chdr = cpackage;

	/* Check that only package after copying has no locations but has
	 * appended string.
	 */
	zassert_equal(chdr[1], 1, NULL);
	zassert_equal(chdr[2], 0, NULL);
	zassert_equal(chdr[3], 0, NULL);

	check_package(package, len, exp_str);
	check_package(cpackage, clen, exp_str);

#undef TEST_FMT
}

/* Store read-only string by index when read-write string is appended. This
 * is supported only by runtime packaging.
 */
static void test_cbprintf_ro_loc_rw_present(void)
{
	static const char *test_str = "test %d %s";
	char test_str1[] = "test str1";
	uint32_t flags = CBPRINTF_PACKAGE_ADD_RO_STR_POS;

#define TEST_FMT test_str, 100, test_str1
	char exp_str[256];

	snprintfcb(exp_str, sizeof(exp_str), TEST_FMT);

	int len = cbprintf_package(NULL, 0, flags, TEST_FMT);

	zassert_true(len > 0, NULL);

	uint8_t __aligned(CBPRINTF_PACKAGE_ALIGNMENT) package[len];

	len = cbprintf_package(package, sizeof(package), flags, TEST_FMT);
	zassert_true(len > 0, NULL);

	uint8_t *hdr = package;

	/* Check that only read-only string location array size is non zero */
	zassert_equal(hdr[1], 1, NULL);
	zassert_equal(hdr[2], 1, NULL);
	zassert_equal(hdr[3], 0, NULL);

	int clen;

	/* Calculate size needed for package with appended read-only strings. */
	clen = cbprintf_package_copy(package, sizeof(package), NULL, 0,
				     CBPRINTF_PACKAGE_COPY_RO_STR, NULL, 0);

	/* Length will be increased by string length + null terminator. */
	zassert_equal(clen, len + (int)strlen(test_str) + 1, NULL);

	uint8_t __aligned(CBPRINTF_PACKAGE_ALIGNMENT) cpackage[clen];

	int clen2 = cbprintf_package_copy(package, sizeof(package), cpackage, sizeof(cpackage),
				     CBPRINTF_PACKAGE_COPY_RO_STR, NULL, 0);

	zassert_equal(clen, clen2, NULL);

	/* Length will be increased by string length + null terminator. */
	zassert_equal(clen, len + (int)strlen(test_str) + 1, NULL);

	uint8_t *chdr = cpackage;

	/* Check that only package after copying has no locations but has
	 * appended string.
	 */
	zassert_equal(chdr[1], 2, NULL);
	zassert_equal(chdr[2], 0, NULL);
	zassert_equal(chdr[3], 0, NULL);

	check_package(package, clen, exp_str);
	check_package(cpackage, clen, exp_str);

#undef TEST_FMT
}

static void test_cbprintf_ro_rw_loc(void)
{
	/* Strings does not need to be in read-only memory section, flag indicates
	 * that n first strings are read only.
	 */
	char test_str[] = "test %s %s %d %s";
	char cstr[] = "const";

	char test_str1[] = "test str1";
	char test_str2[] = "test str2";

#define TEST_FMT test_str, cstr, test_str1, 100, test_str2
	char exp_str[256];

	snprintfcb(exp_str, sizeof(exp_str), TEST_FMT);

	uint32_t flags = CBPRINTF_PACKAGE_FIRST_RO_STR_CNT(1) |
			 CBPRINTF_PACKAGE_ADD_RO_STR_POS |
			 CBPRINTF_PACKAGE_ADD_RW_STR_POS;

	int len = cbprintf_package(NULL, 0, flags, TEST_FMT);
	int slen;

	CBPRINTF_STATIC_PACKAGE(NULL, 0, slen, ALIGN_OFFSET, flags, TEST_FMT);
	zassert_true(len > 0, NULL);
	zassert_equal(len, slen, NULL);

	uint8_t __aligned(CBPRINTF_PACKAGE_ALIGNMENT) package[len];
	uint8_t __aligned(CBPRINTF_PACKAGE_ALIGNMENT) spackage[len];

	memset(package, 0, len);
	memset(spackage, 0, len);

	int len2 = cbprintf_package(package, sizeof(package), flags, TEST_FMT);

	CBPRINTF_STATIC_PACKAGE(spackage, sizeof(spackage), slen, ALIGN_OFFSET, flags, TEST_FMT);
	zassert_equal(len, len2, NULL);
	zassert_equal(slen, len2, NULL);
	zassert_equal(memcmp(package, spackage, len), 0, NULL);

	uint8_t *hdr = package;

	/* Check that expected number of ro and rw locations are present and no
	 * strings appended.
	 */
	zassert_equal(hdr[1], 0, NULL);
	zassert_equal(hdr[2], 2, NULL);
	zassert_equal(hdr[3], 2, NULL);

	int clen;

	uint16_t strl[2] = {0};

	/* Calculate size needed for package with appended read-only strings. */
	clen = cbprintf_package_copy(package, sizeof(package), NULL, 0,
				     CBPRINTF_PACKAGE_COPY_RO_STR, strl, ARRAY_SIZE(strl));

	/* Length will be increased by 2 string lengths + null terminators. */
	zassert_equal(clen, len + (int)strlen(test_str) + (int)strlen(cstr) + 2, NULL);
	zassert_equal(strl[0], strlen(test_str) + 1, NULL);
	zassert_equal(strl[1], strlen(cstr) + 1, NULL);

	uint8_t __aligned(CBPRINTF_PACKAGE_ALIGNMENT) cpackage[clen];

	int clen2 = cbprintf_package_copy(package, sizeof(package), cpackage, sizeof(cpackage),
				     CBPRINTF_PACKAGE_COPY_RO_STR, strl, ARRAY_SIZE(strl));

	zassert_equal(clen, clen2, NULL);

	uint8_t *chdr = cpackage;

	/* Check that read only strings have been appended. */
	zassert_equal(chdr[1], 2, NULL);
	zassert_equal(chdr[2], 0, NULL);
	zassert_equal(chdr[3], 2, NULL);

	check_package(package, len, exp_str);
	check_package(cpackage, clen, exp_str);

	/* Calculate size needed for package with appended read-write strings. */
	clen = cbprintf_package_copy(package, sizeof(package), NULL, 0,
				     CBPRINTF_PACKAGE_COPY_RW_STR, NULL, 0);

	/* Length will be increased by 2 string lengths + null terminators. */
	zassert_equal(clen, len + (int)strlen(test_str1) + (int)strlen(test_str2) + 2, NULL);

	uint8_t __aligned(CBPRINTF_PACKAGE_ALIGNMENT) cpackage2[clen];

	clen2 = cbprintf_package_copy(package, sizeof(package), cpackage2, sizeof(cpackage2),
				     CBPRINTF_PACKAGE_COPY_RW_STR, NULL, 0);

	zassert_equal(clen, clen2, NULL);

	chdr = cpackage2;

	/* Check that read write strings have been appended. */
	zassert_equal(chdr[1], 2, NULL);
	zassert_equal(chdr[2], 2, NULL);
	zassert_equal(chdr[3], 0, NULL);

	check_package(package, len, exp_str);
	check_package(cpackage2, clen, exp_str);

#undef TEST_FMT
}

static void test_cbprintf_ro_rw_loc_const_char_ptr(void)
{
	/* Strings does not need to be in read-only memory section, flag indicates
	 * that n first strings are read only.
	 */
	char test_str[] = "test %s %s %d %s";
	static const char cstr[] = "const";

	char test_str1[] = "test str1";
	static const char test_str2[] = "test str2";

	/* Test skipped for cases where static const data is not located in
	 * read-only section.
	 */
	if (!linker_is_in_rodata(test_str)) {
		ztest_test_skip();
	}

#define TEST_FMT test_str, cstr, test_str1, 100, test_str2
	char exp_str[256];

	snprintfcb(exp_str, sizeof(exp_str), TEST_FMT);


	/* Use flag which is causing all const char pointers to be considered as
	 * read only strings.
	 */
	uint32_t flags = CBPRINTF_PACKAGE_CONST_CHAR_RO |
			 CBPRINTF_PACKAGE_ADD_RO_STR_POS |
			 CBPRINTF_PACKAGE_ADD_RW_STR_POS;

	int len = cbprintf_package(NULL, 0, flags, TEST_FMT);
	int slen;

	CBPRINTF_STATIC_PACKAGE(NULL, 0, slen, ALIGN_OFFSET, flags, TEST_FMT);
	zassert_true(len > 0, NULL);
	zassert_equal(len, slen, NULL);

	uint8_t __aligned(CBPRINTF_PACKAGE_ALIGNMENT) package[len];
	uint8_t __aligned(CBPRINTF_PACKAGE_ALIGNMENT) spackage[len];

	memset(package, 0, len);
	memset(spackage, 0, len);

	int len2 = cbprintf_package(package, sizeof(package), flags, TEST_FMT);

	CBPRINTF_STATIC_PACKAGE(spackage, sizeof(spackage), slen, ALIGN_OFFSET, flags, TEST_FMT);
	zassert_equal(len, len2, NULL);
	zassert_equal(slen, len2, NULL);
	zassert_equal(memcmp(package, spackage, len), 0, NULL);

	uint8_t *hdr = package;

	/* Check that expected number of ro and rw locations are present and no
	 * strings appended.
	 */
	zassert_equal(hdr[1], 0, NULL);
	zassert_equal(hdr[2], 3, NULL);
	zassert_equal(hdr[3], 1, NULL);

	int clen;

	/* Calculate size needed for package with appended read-only strings. */
	clen = cbprintf_package_copy(package, sizeof(package), NULL, 0,
				     CBPRINTF_PACKAGE_COPY_RO_STR, NULL, 0);

	/* Length will be increased by 2 string lengths + null terminators. */
	size_t str_append_len = (int)strlen(test_str) +
				(int)strlen(cstr) +
				(int)strlen(test_str2) + 3;
	zassert_equal(clen, len + (int)str_append_len, NULL);

	uint8_t __aligned(CBPRINTF_PACKAGE_ALIGNMENT) cpackage[clen];

	int clen2 = cbprintf_package_copy(package, sizeof(package), cpackage, sizeof(cpackage),
				     CBPRINTF_PACKAGE_COPY_RO_STR, NULL, 0);

	zassert_equal(clen, clen2, NULL);

	uint8_t *chdr = cpackage;

	/* Check that read only strings have been appended. */
	zassert_equal(chdr[1], 3, NULL);
	zassert_equal(chdr[2], 0, NULL);
	zassert_equal(chdr[3], 1, NULL);

	check_package(package, len, exp_str);
	check_package(cpackage, clen, exp_str);

	/* Calculate size needed for package with appended read-write strings. */
	clen = cbprintf_package_copy(package, sizeof(package), NULL, 0,
				     CBPRINTF_PACKAGE_COPY_RW_STR, NULL, 0);

	/* Length will be increased by 1 string length + null terminator. */
	zassert_equal(clen, len + (int)strlen(test_str1) + 1, NULL);

	uint8_t __aligned(CBPRINTF_PACKAGE_ALIGNMENT) cpackage2[clen];

	clen2 = cbprintf_package_copy(package, sizeof(package), cpackage2, sizeof(cpackage2),
				     CBPRINTF_PACKAGE_COPY_RW_STR, NULL, 0);

	zassert_equal(clen, clen2, NULL);

	chdr = cpackage2;

	/* Check that read write strings have been appended. */
	zassert_equal(chdr[1], 1, NULL);
	zassert_equal(chdr[2], 3, NULL);
	zassert_equal(chdr[3], 0, NULL);

	check_package(package, len, exp_str);
	check_package(cpackage2, clen, exp_str);
#undef TEST_FMT
}

static void test_cbprintf_rw_loc_const_char_ptr(void)
{
	/* Test requires that static packaging is applied. Runtime packaging
	 * cannot be tricked because it checks pointers against read only
	 * section.
	 */
	if (Z_C_GENERIC == 0) {
		ztest_test_skip();
	}

	int slen, slen2;
	int clen, clen2;
	static const char test_str[] = "test %s %d %s";
	char test_str1[] = "test str1";
	static const char test_str2[] = "test str2";
	/* Store indexes of rw strings. */
	uint32_t flags = CBPRINTF_PACKAGE_ADD_RW_STR_POS;
	uint8_t *hdr;

	/* Test skipped for cases where static const data is not located in
	 * read-only section.
	 */
	if (!linker_is_in_rodata(test_str)) {
		ztest_test_skip();
	}

#define TEST_FMT test_str, test_str1, 100, test_str2
	char exp_str[256];

	snprintfcb(exp_str, sizeof(exp_str), TEST_FMT);

	CBPRINTF_STATIC_PACKAGE(NULL, 0, slen, ALIGN_OFFSET, flags, TEST_FMT);
	zassert_true(slen > 0, NULL);

	uint8_t __aligned(CBPRINTF_PACKAGE_ALIGNMENT) spackage[slen];

	memset(spackage, 0, slen);

	CBPRINTF_STATIC_PACKAGE(spackage, sizeof(spackage), slen2, ALIGN_OFFSET, flags, TEST_FMT);
	zassert_equal(slen, slen2, NULL);

	hdr = spackage;

	/* Check that expected number of ro and rw locations are present and no
	 * strings appended.
	 */
	zassert_equal(hdr[1], 0, NULL);
	zassert_equal(hdr[2], 0, NULL);
	zassert_equal(hdr[3], 2, NULL);

	/* Calculate size needed for package with appended read-only strings. */
	clen = cbprintf_package_copy(spackage, sizeof(spackage), NULL, 0,
				     CBPRINTF_PACKAGE_COPY_RW_STR, NULL, 0);

	/* Length will be increased by 2 string lengths + null terminators. */
	zassert_equal(clen, slen + (int)strlen(test_str1) + 1, NULL);

	uint8_t __aligned(CBPRINTF_PACKAGE_ALIGNMENT) cpackage[clen];

	clen2 = cbprintf_package_copy(spackage, sizeof(spackage), cpackage, sizeof(cpackage),
				     CBPRINTF_PACKAGE_COPY_RW_STR, NULL, 0);

	zassert_equal(clen, clen2, NULL);

	hdr = cpackage;

	/* Check that one string has been appended. */
	zassert_equal(hdr[1], 1, NULL);
	zassert_equal(hdr[2], 0, NULL);
	zassert_equal(hdr[3], 1, NULL);

	check_package(spackage, slen, exp_str);
	test_str1[0] = '\0';
	check_package(cpackage, clen, exp_str);
#undef TEST_FMT
}

static void test_cbprintf_must_runtime_package(void)
{
	int rv;

	if (Z_C_GENERIC == 0) {
		ztest_test_skip();
	}

	rv = CBPRINTF_MUST_RUNTIME_PACKAGE(0, "test");
	zassert_equal(rv, 0, NULL);

	rv = CBPRINTF_MUST_RUNTIME_PACKAGE(0, "test %x", 100);
	zassert_equal(rv, 0, NULL);

	rv = CBPRINTF_MUST_RUNTIME_PACKAGE(0, "test %x %s", 100, "");
	zassert_equal(rv, 1, NULL);

	rv = CBPRINTF_MUST_RUNTIME_PACKAGE(CBPRINTF_PACKAGE_CONST_CHAR_RO, "test %x", 100);
	zassert_equal(rv, 0, NULL);

	rv = CBPRINTF_MUST_RUNTIME_PACKAGE(CBPRINTF_PACKAGE_CONST_CHAR_RO,
					   "test %x %s", 100, (const char *)"s");
	zassert_equal(rv, 0, NULL);

	rv = CBPRINTF_MUST_RUNTIME_PACKAGE(CBPRINTF_PACKAGE_CONST_CHAR_RO,
					   "test %x %s %s", 100, (char *)"s", (const char *)"foo");
	zassert_equal(rv, 1, NULL);

	rv = CBPRINTF_MUST_RUNTIME_PACKAGE(CBPRINTF_PACKAGE_FIRST_RO_STR_CNT(1),
					   "test %s", (char *)"s");
	zassert_equal(rv, 0, NULL);

	rv = CBPRINTF_MUST_RUNTIME_PACKAGE(CBPRINTF_PACKAGE_FIRST_RO_STR_CNT(2),
					   "test %s %s %d", (const char *)"s", (char *)"s", 10);
	zassert_equal(rv, 0, NULL);

	rv = CBPRINTF_MUST_RUNTIME_PACKAGE(CBPRINTF_PACKAGE_FIRST_RO_STR_CNT(2),
					   "test %s %s %s", (const char *)"s", (char *)"s", "s");
	zassert_equal(rv, 1, NULL);

	rv = CBPRINTF_MUST_RUNTIME_PACKAGE(CBPRINTF_PACKAGE_FIRST_RO_STR_CNT(1) |
					   CBPRINTF_PACKAGE_CONST_CHAR_RO,
					   "test %s %s %d", (char *)"s", (const char *)"s", 10);
	zassert_equal(rv, 0, NULL);

	/* When RW str positions are stored static packaging can always be used */
	rv = CBPRINTF_MUST_RUNTIME_PACKAGE(CBPRINTF_PACKAGE_ADD_RW_STR_POS,
					   "test %s %s %d", (char *)"s", (const char *)"s", 10);
	zassert_equal(rv, 0, NULL);
}

void test_main(void)
{
#ifdef __cplusplus
	printk("C++\n");
#else
	printk("sizeof:  int=%zu long=%zu ptr=%zu long long=%zu double=%zu long double=%zu\n",
	       sizeof(int), sizeof(long), sizeof(void *), sizeof(long long),
	       sizeof(double), sizeof(long double));
	printk("alignof: int=%zu long=%zu ptr=%zu long long=%zu double=%zu long double=%zu\n",
	       __alignof__(int), __alignof__(long), __alignof__(void *),
	       __alignof__(long long), __alignof__(double), __alignof__(long double));
	printk("%s C11 _Generic\n", Z_C_GENERIC ? "With" : "Without");
#endif

	ztest_test_suite(cbprintf_package,
			 ztest_unit_test(test_cbprintf_package),
			 ztest_unit_test(test_cbprintf_rw_str_indexes),
			 ztest_unit_test(test_cbprintf_fsc_package),
			 ztest_unit_test(test_cbprintf_ro_loc),
			 ztest_unit_test(test_cbprintf_ro_loc_rw_present),
			 ztest_unit_test(test_cbprintf_ro_rw_loc),
			 ztest_unit_test(test_cbprintf_ro_rw_loc_const_char_ptr),
			 ztest_unit_test(test_cbprintf_rw_loc_const_char_ptr),
			 ztest_unit_test(test_cbprintf_must_runtime_package)
			 );

	ztest_run_test_suite(cbprintf_package);
}
