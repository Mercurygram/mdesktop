'''
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
'''

# Generates the application-level ("decrypted") layer of the secret chat
# protocol (mtproto/scheme/secret.tl) into a dedicated `decrypted::` namespace
# so its constructors do not collide with the global MTP types generated from
# api.tl / mtproto.tl by codegen_scheme.py.

import glob, re, binascii, os, sys

sys.dont_write_bytecode = True
scriptPath = os.path.dirname(os.path.realpath(__file__))
sys.path.append(scriptPath + '/../../../lib_tl/tl')
from generate_tl import generate

generate({
  'namespaces': {
    'global': 'decrypted',
    'creator': 'details',
  },
  'prefixes': {
    'type': 'MTP',
    'data': 'MTPD',
    'id': 'mtpc',
    'construct': 'MTP_',
  },
  'types': {
    'prime': 'mtpPrime',
    'typeId': 'mtpTypeId',
    'buffer': 'mtpBuffer',
  },
  'sections': [
    'read-write',
  ],

  # The decrypted layer has no flag-inheritance relations.
  'flagInheritance': {},

  # Canonical (wire) ids that differ from the crc32 of the normalized line,
  # exactly as peers send them on the wire. Keep these verbatim.
  'typeIdExceptions': [
    'decryptedMessageMediaDocument#6abd9782',
  ],

  'renamedTypes': {},

  'skip': [
    'int ? = Int;',
    'long ? = Long;',
    'double ? = Double;',
    'string ? = String;',

    'vector {t:Type} # [ t ] = Vector t;',

    'int128 4*[ int ] = Int128;',
    'int256 8*[ int ] = Int256;',

    'vector#1cb5c415 {t:Type} # [ t ] = Vector t;',
  ],
  'builtin': [
    'int',
    'long',
    'double',
    'string',
    'bytes',
    'int128',
    'int256',
  ],
  'builtinTemplates': [
    'vector',
    'flags',
  ],
  'synonyms': {
    'bytes': 'string',
  },
  'builtinInclude': 'mtproto/core_types.h',
  'optimizeSingleData': True,

})
