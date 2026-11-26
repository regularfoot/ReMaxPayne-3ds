"""Read animation transition data from the supplied Xbox disc."""
import argparse
import hashlib
from pathlib import Path
import re
import struct

from pack_native import member
from xbox_disc import REVISIONS, archive_root

ACTION_HEADER_SHA = '1bd37311d4f9bf897d1c2942ca59f467f1c5234891154f5402480cb0883181ce'
RULE_COUNT = 447
RULE_SIZE = 24

# XBE virtual addresses of the rule constructor, tree allocator, tree insert,
# and two adjacent configuration globals for each verified executable.
LAYOUTS = {
    'PAL': (0x1BA7D0, 0x1B9DE0, 0x1BA330, 0x521404),
    'NTSC': (0x1B68E0, 0x1B5EF0, 0x1B6440, 0x519D2C),
}


def transition_data(executable, action_ids):
    from unicorn import Uc, UC_ARCH_X86, UC_HOOK_CODE, UC_MODE_32, UcError
    from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EIP, UC_X86_REG_ESP

    image = executable.read_bytes()
    digest = hashlib.sha256(image).hexdigest()
    if digest not in REVISIONS:
        raise ValueError(f'Unsupported default.xbe revision: SHA-256 {digest}')
    constructor, allocate_tree, insert_pair, config_globals = LAYOUTS[REVISIONS[digest]]

    cpu = Uc(UC_ARCH_X86, UC_MODE_32)
    image_limit = 0x800000
    cpu.mem_map(0, image_limit)
    base = struct.unpack_from('<I', image, 0x104)[0]
    section_count, section_table = struct.unpack_from('<II', image, 0x11C)
    for index in range(section_count):
        entry = section_table - base + 56 * index
        address, _, offset, length = struct.unpack_from('<4I', image, entry + 4)
        if not length:
            continue
        if address < base or address + length > image_limit or offset + length > len(image):
            raise ValueError(f'Invalid XBE section {index}')
        cpu.mem_write(address, image[offset:offset + length])

    rule_object = 0x1000000
    config = rule_object + 0x7000
    finished = rule_object + 0x8000
    stack = rule_object + 0xF000
    cpu.mem_map(rule_object, 0x10000)
    cpu.mem_write(stack, struct.pack('<I', finished))
    cpu.reg_write(UC_X86_REG_ESP, stack)
    cpu.reg_write(UC_X86_REG_ECX, rule_object)

    shoot_start = action_ids['shootempty']
    weapon_types = action_ids['shootpainkiller'] - shoot_start + 1
    cpu.mem_write(config_globals, struct.pack('<II', weapon_types, config))
    cpu.mem_write(config + 8, struct.pack('<I', shoot_start))
    cpu.mem_write(config + 0x14, struct.pack('<I', action_ids['reloadempty']))

    # Run only the constructor in memory. Supply dummy tree headers and collect
    # insertion keys directly, so the Xbox allocator and OS are never called.
    trees = {rule_object + 0x2A30: set(), rule_object + 0x2A3C: set()}

    def tree_hook(uc, address, _size, _user_data):
        sp = uc.reg_read(UC_X86_REG_ESP)
        tree = uc.reg_read(UC_X86_REG_ECX)
        return_address, result_ptr, pair_ptr = struct.unpack('<3I', uc.mem_read(sp, 12))
        if address == allocate_tree:
            result_ptr = rule_object + (0x5000 if tree == rule_object + 0x2A30 else 0x5100)
            arg_bytes = 0
        else:
            trees[tree].add(struct.unpack('<II', uc.mem_read(pair_ptr, 8)))
            arg_bytes = 8
        uc.reg_write(UC_X86_REG_EAX, result_ptr)
        uc.reg_write(UC_X86_REG_ESP, sp + 4 + arg_bytes)
        uc.reg_write(UC_X86_REG_EIP, return_address)

    for address in (allocate_tree, insert_pair):
        cpu.hook_add(UC_HOOK_CODE, tree_hook, begin=address, end=address)
    try:
        cpu.emu_start(constructor, finished, count=100_000)
    except UcError as error:
        raise ValueError('Could not extract Xbox animation transition rules') from error
    if cpu.reg_read(UC_X86_REG_EIP) != finished:
        raise ValueError('Xbox animation rule constructor did not finish')

    copy_pairs, phase_pairs = map(sorted, trees.values())
    pairs = copy_pairs + phase_pairs
    if (len(copy_pairs), len(phase_pairs)) != (32, 144) or any(
        not 0 <= first <= second < RULE_COUNT for first, second in pairs
    ):
        raise ValueError('Invalid animation transition pairs')
    rules = bytes(cpu.mem_read(rule_object + 8, RULE_COUNT * RULE_SIZE))
    return rules, copy_pairs, phase_pairs


def encode(disc, folder):
    header = member(archive_root(disc) / 'x_headers.ras', 'data/database/characteranimid.h')
    if hashlib.sha256(header).hexdigest() != ACTION_HEADER_SHA:
        raise ValueError('Unsupported Xbox animation action IDs')
    action_ids = {
        key.lower(): int(value, 0)
        for key, value in re.findall(r'#define\s+CHARANIM_(\w+)\s+(0x[0-9a-fA-F]+|[0-9]+)', header.decode('latin1'))
    }
    rules, copy_pairs, phase_pairs = transition_data(disc / 'default.xbe', action_ids)
    index = (folder / 'poses.idx').read_bytes()
    action_count = struct.unpack_from('<I', index, 8)[0]
    mapping = []
    for action in range(action_count):
        offset = 16 + 64 * action
        key = index[offset:offset + 64].split(b'\0', 1)[0].decode()
        value = action_ids.get(key, RULE_COUNT)
        if not 0 <= value < RULE_COUNT:
            raise ValueError(f'Missing Xbox transition rule for {key}')
        mapping.append(value)
    return (
        struct.pack('<4sIIII', b'TRN1', RULE_COUNT, action_count, len(copy_pairs), len(phase_pairs))
        + rules
        + struct.pack(f'<{action_count}H', *mapping)
        + b''.join(struct.pack('<HH', *pair) for pair in copy_pairs + phase_pairs)
    )


def pack(disc, folder):
    data = encode(disc, folder)
    (folder / 'transitions.bin').write_bytes(data)
    print('Packed animation transition rules from the Xbox disc')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('disc', type=Path)
    parser.add_argument('folder', type=Path)
    args = parser.parse_args()
    pack(args.disc, args.folder)
