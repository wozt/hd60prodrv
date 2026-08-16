#!/usr/bin/env python3
import argparse
import re
from pathlib import Path

from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM
from elftools.elf.elffile import ELFFile


def section_by_name(elf, name):
    sec = elf.get_section_by_name(name)
    if sec is None:
        raise SystemExit(f"missing section {name}")
    return sec


def section_index_by_name(elf, name):
    for idx, sec in enumerate(elf.iter_sections()):
        if sec.name == name:
            return idx
    return None


def load_symbols(elf):
    symbols = {}
    for tab_name in (".symtab", ".dynsym"):
        symtab = elf.get_section_by_name(tab_name)
        if symtab is None:
            continue
        for sym in symtab.iter_symbols():
            name = sym.name
            if name:
                symbols.setdefault(name, sym)
    return symbols


def symbol_name_by_value(symbols, value):
    best_name = None
    best_start = None
    for name, sym in symbols.items():
        start = sym["st_value"]
        size = sym["st_size"]
        if start == 0:
            continue
        if (size == 0 and value == start) or (size and start <= value < start + size):
            if best_start is None or start > best_start:
                best_name = name
                best_start = start
    return best_name


def symbol_name_at(elf, section_index, value):
    symtab = elf.get_section_by_name(".symtab")
    if symtab is None:
        return None
    best = None
    for sym in symtab.iter_symbols():
        if sym["st_shndx"] != section_index:
            continue
        start = sym["st_value"]
        size = sym["st_size"]
        if start <= value and (size == 0 or value < start + size):
            if best is None or start > best["st_value"]:
                best = sym
    return best.name if best is not None and best.name else None


def build_reloc_map(elf, target_section_name):
	target = section_by_name(elf, target_section_name)
	rel = elf.get_section_by_name(".rel" + target_section_name)
	if rel is None:
		return {}
	symtab = elf.get_section(rel["sh_link"])
	target_data = target.data()
	out = {}
	for reloc in rel.iter_relocations():
		sym = symtab.get_symbol(reloc["r_info_sym"])
		off = reloc["r_offset"]
		addend = 0
		if 0 <= off <= len(target_data) - 4:
			addend = int.from_bytes(target_data[off:off + 4], "little")
		out[reloc["r_offset"]] = (
			sym.name or f"section_{sym['st_shndx']}",
			sym["st_value"] + addend,
			sym["st_shndx"],
		)
	return out


def section_name(elf, section_index):
    if isinstance(section_index, str):
        return section_index
    sec = elf.get_section(section_index)
    return sec.name if sec is not None else f"section_{section_index}"


def string_at(elf, section_index, value):
    if isinstance(section_index, str):
        return None
    sec = elf.get_section(section_index)
    if sec is None or not sec.name.startswith(".rodata"):
        return None
    data = sec.data()
    if value >= len(data):
        return None
    end = data.find(b"\x00", value)
    if end < 0:
        end = min(len(data), value + 80)
    if end <= value:
        return None
    try:
        out = data[value:end].decode("ascii", "replace")
    except Exception:
        return None
    if not out or any(ord(c) < 0x20 and c not in "\t" for c in out):
        return None
    return out


def rodata_string_at(elf, value):
    for sec in elf.iter_sections():
        if not (sec.name.startswith(".rodata") or
                sec.name in ("__ksymtab_strings", ".modinfo")):
            continue
        start = sec["sh_addr"]
        size = sec["sh_size"]
        if start <= value < start + size:
            data = sec.data()
            off = value - start
            end = data.find(b"\x00", off)
            if end < 0:
                end = min(len(data), off + 80)
            try:
                return data[off:end].decode("ascii", "replace")
            except Exception:
                return None
    return None


def parse_arm_pc_literal(op_str):
    m = re.match(r"r\d+,\s*\[pc,\s*#(?P<sign>-?)(?P<imm>0x[0-9a-fA-F]+|\d+)\]$", op_str)
    if not m:
        return None
    imm = int(m.group("imm"), 0)
    if m.group("sign"):
        imm = -imm
    return imm


def read_u32_at_section_addr(sec, addr):
    off = addr - sec["sh_addr"]
    if off < 0 or off > sec["sh_size"] - 4:
        return None
    return int.from_bytes(sec.data()[off:off + 4], "little")


def reloc_note(elf, sym_name, sym_value, sym_shndx):
    sec_name = section_name(elf, sym_shndx)
    extra = string_at(elf, sym_shndx, sym_value)
    if extra:
        return f"{sym_name}@{sec_name}+0x{sym_value:x} \"{extra}\""
    return f"{sym_name}@{sec_name}+0x{sym_value:x}"


def disasm_function(elf, text, text_index, symbols, relocs, name):
    if name in ("all", ".text"):
        start = text["sh_addr"]
        size = text["sh_size"]
    else:
        sym = symbols.get(name)
        if sym is None:
            print(f"\n== {name} missing ==")
            return
        start = sym["st_value"]
        size = sym["st_size"]
    text_start = text["sh_addr"]
    text_end = text_start + text["sh_size"]
    if start < text_start or start >= text_end:
        print(f"\n== {name} outside .text: 0x{start:x} ==")
        return
    off = start - text_start
    data = text.data()[off:off + size]
    md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    md.detail = False
    print(f"\n== {name} 0x{start:x} size {size} ==")
    for insn in md.disasm(data, start):
        note = []
        for off in range(insn.address, insn.address + insn.size):
            if off in relocs:
                sym_name, sym_value, sym_shndx = relocs[off]
                note.append("reloc->" + reloc_note(elf, sym_name, sym_value, sym_shndx))
        if insn.mnemonic == "ldr":
            imm = parse_arm_pc_literal(insn.op_str)
            if imm is not None:
                literal_addr = insn.address + 8 + imm
                literal_value = read_u32_at_section_addr(text, literal_addr)
                if literal_value is not None:
                    literal = f"literal@0x{literal_addr:x}=0x{literal_value:08x}"
                    if literal_addr in relocs:
                        sym_name, sym_value, sym_shndx = relocs[literal_addr]
                        literal += " -> " + reloc_note(elf, sym_name, sym_value, sym_shndx)
                    else:
                        text_sym = symbol_name_at(elf, text_index, literal_value)
                        ro = rodata_string_at(elf, literal_value)
                        if text_sym:
                            literal += f" -> .text:{text_sym}+0x{literal_value:x}"
                        elif ro:
                            literal += f" -> \"{ro}\""
                    note.append(literal)
        if insn.mnemonic.startswith("bl") or insn.mnemonic == "b":
            m = re.match(r"#(0x[0-9a-fA-F]+|\d+)$", insn.op_str)
            if m:
                target = int(m.group(1), 0)
                target_name = symbol_name_by_value(symbols, target)
                if target_name:
                    note.append(f"branch->{target_name}@0x{target:x}")
        op = insn.op_str
        print(f"0x{insn.address:04x}: {insn.mnemonic:<8} {op:<32}" +
              (f" ; {', '.join(note)}" if note else ""))


def dump_rodata_table(elf, symbols, name):
    sym = symbols.get(name)
    if sym is None:
        return
    sec = elf.get_section(sym["st_shndx"])
    off = sym["st_value"] - sec["sh_addr"]
    data = sec.data()[off:off + sym["st_size"]]
    nonzero = [(i, b) for i, b in enumerate(data) if b]
    print(f"\n== {name} size {len(data)} ==")
    if nonzero:
        for i, b in nonzero:
            print(f"{name}[{i}] = 0x{b:02x}")
    else:
        print("all zero")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ko", type=Path)
    ap.add_argument("functions", nargs="*")
    args = ap.parse_args()

    with args.ko.open("rb") as f:
        elf = ELFFile(f)
        text = section_by_name(elf, ".text")
        text_index = section_index_by_name(elf, ".text")
        symbols = load_symbols(elf)
        relocs = build_reloc_map(elf, ".text")
        funcs = args.functions or [
            "command_store", "hready_store", "epint_store",
            "store_channel_done", "pcie_set_outbound", "pciep_isr",
        ]
        for name in funcs:
            disasm_function(elf, text, text_index, symbols, relocs, name)
        for name in ("ep_cmds_size", "ep_fw_update_size",
                     "ep_ints_size", "ep_ints_1080p_size", "ep_aic_size"):
            dump_rodata_table(elf, symbols, name)


if __name__ == "__main__":
    main()
