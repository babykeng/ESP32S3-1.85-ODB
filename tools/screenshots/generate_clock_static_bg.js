#!/usr/bin/env node

const fs = require("fs");
const path = require("path");

const inputDir = process.argv[2] || "/tmp/clock_bg";
const outputPath = process.argv[3] || path.join(__dirname, "../../components/esp32s3_obd/images/clockStaticBg.c");

function readToken(buf, state) {
    while(state.pos < buf.length) {
        if(buf[state.pos] === 0x23) {
            while(state.pos < buf.length && buf[state.pos++] !== 0x0A) {}
        } else if(buf[state.pos] <= 0x20) {
            state.pos++;
        } else {
            break;
        }
    }

    const start = state.pos;
    while(state.pos < buf.length && buf[state.pos] > 0x20) state.pos++;
    return buf.slice(start, state.pos).toString("ascii");
}

function readPpm(filePath) {
    const buf = fs.readFileSync(filePath);
    const state = { pos: 0 };
    const magic = readToken(buf, state);
    const width = Number(readToken(buf, state));
    const height = Number(readToken(buf, state));
    const maxValue = Number(readToken(buf, state));
    while(state.pos < buf.length && buf[state.pos] <= 0x20) state.pos++;

    if(magic !== "P6" || width !== 360 || height !== 360 || maxValue !== 255) {
        throw new Error(`unsupported PPM: ${filePath}`);
    }

    return buf.slice(state.pos, state.pos + width * height * 3);
}

function rgbToLvglSwapped565(red, green, blue) {
    return ((red & 0xF8) |
            ((green & 0xE0) >> 5) |
            ((green & 0x1C) << 11) |
            ((blue & 0xF8) << 5)) & 0xFFFF;
}

function appendByte(out, line, value) {
    const text = `0x${value.toString(16).padStart(2, "0")}, `;
    if(line.value.length + text.length > 118) {
        out.push(`${line.value.trimEnd()}\n`);
        line.value = "  ";
    }
    line.value += text;
}

const out = [];
out.push("// Auto-generated from LVGL Clock static layer screenshots.\n");
out.push("// Rebuild with tools/screenshots/generate_clock_static_bg.js after Clock static UI changes.\n\n");
out.push("#ifdef __has_include\n");
out.push("    #if __has_include(\"lvgl.h\")\n");
out.push("        #ifndef LV_LVGL_H_INCLUDE_SIMPLE\n");
out.push("            #define LV_LVGL_H_INCLUDE_SIMPLE\n");
out.push("        #endif\n");
out.push("    #endif\n");
out.push("#endif\n\n");
out.push("#if defined(LV_LVGL_H_INCLUDE_SIMPLE)\n#include \"lvgl.h\"\n#else\n#include \"lvgl/lvgl.h\"\n#endif\n\n");
out.push("#ifndef LV_ATTRIBUTE_IMG_CLOCKSTATICBG\n#define LV_ATTRIBUTE_IMG_CLOCKSTATICBG\n#endif\n\n");

for(let theme = 0; theme < 4; theme++) {
    const rgb = readPpm(path.join(inputDir, `theme${theme}.ppm`));
    out.push(`const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_CLOCKSTATICBG uint8_t clockStaticBgTheme${theme}_map[] = {\n`);

    const line = { value: "  " };
    for(let i = 0; i < rgb.length; i += 3) {
        const full = rgbToLvglSwapped565(rgb[i], rgb[i + 1], rgb[i + 2]);
        appendByte(out, line, full & 0xFF);
        appendByte(out, line, (full >> 8) & 0xFF);
    }
    if(line.value.trim().length > 0) out.push(`${line.value.trimEnd()}\n`);

    out.push("};\n\n");
    out.push(`const lv_img_dsc_t clockStaticBgTheme${theme} = {\n`);
    out.push("  .header.cf = LV_IMG_CF_TRUE_COLOR,\n");
    out.push("  .header.always_zero = 0,\n");
    out.push("  .header.reserved = 0,\n");
    out.push("  .header.w = 360,\n");
    out.push("  .header.h = 360,\n");
    out.push("  .data_size = 259200,\n");
    out.push(`  .data = clockStaticBgTheme${theme}_map,\n`);
    out.push("};\n\n");
}

fs.writeFileSync(outputPath, out.join(""));
console.log(`wrote ${outputPath}`);
