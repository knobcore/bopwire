// Regression tests for the Linux folder/file picker.
//
// The bug these guard: inside an AppImage, file_picker shells out to the
// host's zenity while LD_LIBRARY_PATH still points at the bundled GTK
// stack, and it locates that binary with `which`. Either step can fail —
// a host whose GLib differs from the bundled one, or a machine with no
// dialog program at all — and in both cases file_picker reported "user
// cancelled", so every picker button in the app did nothing at all with
// no message.

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:bopwire_player/src/util/file_dialogs.dart';

void main() {
  group('environment sanitation', () {
    test('strips the loader vars an AppImage injects', () {
      final sanitized = sanitizeEnvironmentForTest({
        'PATH': '/appdir/usr/bin:/usr/bin',
        'LD_LIBRARY_PATH': '/appdir/usr/lib',
        'LD_PRELOAD': '/appdir/usr/lib/libfoo.so',
        'GDK_PIXBUF_MODULE_FILE': '/appdir/loaders.cache',
        'GIO_MODULE_DIR': '/appdir/gio',
        'GTK_PATH': '/appdir/gtk',
        'HOME': '/home/lain',
        'WAYLAND_DISPLAY': 'wayland-1',
      });

      // The whole point: none of these may reach the host binary.
      expect(sanitized.containsKey('LD_LIBRARY_PATH'), isFalse);
      expect(sanitized.containsKey('LD_PRELOAD'), isFalse);
      expect(sanitized.containsKey('GDK_PIXBUF_MODULE_FILE'), isFalse);
      expect(sanitized.containsKey('GIO_MODULE_DIR'), isFalse);
      expect(sanitized.containsKey('GTK_PATH'), isFalse);

      // ...while the session vars the dialog genuinely needs survive.
      expect(sanitized['HOME'], '/home/lain');
      expect(sanitized['WAYLAND_DISPLAY'], 'wayland-1');
    });

    test('restores the pre-AppImage PATH and LD_LIBRARY_PATH', () {
      final sanitized = sanitizeEnvironmentForTest({
        'PATH': '/appdir/usr/bin:/usr/bin',
        'LD_LIBRARY_PATH': '/appdir/usr/lib',
        'BOPWIRE_HOST_PATH': '/usr/bin:/usr/local/bin',
        'BOPWIRE_HOST_LD_LIBRARY_PATH': '/opt/host/lib',
      });

      expect(sanitized['PATH'], '/usr/bin:/usr/local/bin');
      expect(sanitized['LD_LIBRARY_PATH'], '/opt/host/lib');
      // Our own bookkeeping must not leak downstream.
      expect(sanitized.containsKey('BOPWIRE_HOST_PATH'), isFalse);
      expect(sanitized.containsKey('BOPWIRE_HOST_LD_LIBRARY_PATH'), isFalse);
    });

    test('drops AppImage runtime vars', () {
      final sanitized = sanitizeEnvironmentForTest({
        'PATH': '/usr/bin',
        'APPDIR': '/tmp/.mount_x/',
        'APPIMAGE': '/home/lain/app.AppImage',
        'ARGV0': 'app.AppImage',
      });
      expect(sanitized.containsKey('APPDIR'), isFalse);
      expect(sanitized.containsKey('APPIMAGE'), isFalse);
      expect(sanitized.containsKey('ARGV0'), isFalse);
    });

    test('an empty host backup does not blank out PATH', () {
      final sanitized = sanitizeEnvironmentForTest({
        'PATH': '/usr/bin',
        'BOPWIRE_HOST_PATH': '',
      });
      expect(sanitized['PATH'], '/usr/bin');
    });
  });

  group('dialog resolution', () {
    test('returns null when no dialog program is on PATH', () {
      // This is the case that produced the silent dead button: file_picker
      // threw out of an unawaited onPressed callback. We must instead
      // report it, so resolution returning null is the contract.
      expect(resolveDialogForTest({'PATH': '/nonexistent-dir'}), isNull);
      expect(resolveDialogForTest({'PATH': ''}), isNull);
      expect(resolveDialogForTest(const {}), isNull);
    });

    test('never shells out to `which`', () {
      // A machine without `which` used to raise ProcessException, which
      // was the same dead button. Resolution is pure filesystem probing,
      // so a PATH containing only `which` must still resolve nothing.
      final tmp = Directory.systemTemp.createTempSync('dialogprobe');
      addTearDown(() => tmp.deleteSync(recursive: true));
      File('${tmp.path}/which').writeAsStringSync('#!/bin/sh\n');
      expect(resolveDialogForTest({'PATH': tmp.path}), isNull);
    });

    test('finds an executable dialog and ignores a non-executable one', () {
      final tmp = Directory.systemTemp.createTempSync('dialogprobe');
      addTearDown(() => tmp.deleteSync(recursive: true));

      final zenity = File('${tmp.path}/zenity')..writeAsStringSync('#!/bin/sh\n');
      // Present but not executable -> must not be selected.
      Process.runSync('chmod', ['644', zenity.path]);
      expect(resolveDialogForTest({'PATH': tmp.path}), isNull);

      Process.runSync('chmod', ['755', zenity.path]);
      expect(resolveDialogForTest({'PATH': tmp.path}), 'zenity');
    });

    test('prefers kdialog on KDE, zenity elsewhere', () {
      final tmp = Directory.systemTemp.createTempSync('dialogprobe');
      addTearDown(() => tmp.deleteSync(recursive: true));
      for (final n in ['zenity', 'kdialog']) {
        File('${tmp.path}/$n').writeAsStringSync('#!/bin/sh\n');
        Process.runSync('chmod', ['755', '${tmp.path}/$n']);
      }

      expect(
        resolveDialogForTest({'PATH': tmp.path, 'XDG_CURRENT_DESKTOP': 'KDE'}),
        'kdialog',
      );
      expect(
        resolveDialogForTest({'PATH': tmp.path, 'XDG_CURRENT_DESKTOP': 'GNOME'}),
        'zenity',
      );
    });
  });

  test('the unavailable message tells the user what to install', () {
    // If this ever goes vague the bug regresses into a different kind of
    // dead end: a message that does not say what to do.
    expect(noDialogMessageForTest, contains('zenity'));
    expect(noDialogMessageForTest, contains('kdialog'));
  });
}
