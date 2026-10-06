// Embeds the two DejaVu Sans TTFs directly into libhds_native.so's own .rodata via the
// assembler's .incbin directive -- avoids AAssetManager/JNI asset-passing plumbing entirely (no
// opening the .apk's assets/ via a Java-side AssetManager, no passing an fd or byte[] across
// JNI): the font bytes just become part of the shared library's own image, available to
// hds_android.c as plain extern symbols the moment the library is dlopen()'d, no different from
// a big static array.
//
// The font FILES themselves live at third_party/dejavu-fonts/ (repo root, NOT copied into this
// android/ tree -- exactly one copy of each file). HDS_FONT_REGULAR_PATH/HDS_FONT_BOLD_PATH are
// passed in as fully-quoted -D defines by whichever build drives this (tools/build_android_manual
// .sh and/or CMakeLists.txt), both pointing at that one location. This file has the .S (capital)
// extension specifically so it goes through the C preprocessor before assembly -- a -D value can
// then stand in for a path here at all, since .incbin's own argument is a single string literal
// (it doesn't concatenate two), so the define must already be the complete quoted path.

.section .rodata

.global hds_font_dejavu_sans_regular_start
.global hds_font_dejavu_sans_regular_end
.balign 4
hds_font_dejavu_sans_regular_start:
.incbin HDS_FONT_REGULAR_PATH
hds_font_dejavu_sans_regular_end:

.global hds_font_dejavu_sans_bold_start
.global hds_font_dejavu_sans_bold_end
.balign 4
hds_font_dejavu_sans_bold_start:
.incbin HDS_FONT_BOLD_PATH
hds_font_dejavu_sans_bold_end:
