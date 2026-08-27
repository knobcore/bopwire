// Cross-platform file/folder chooser helpers.
//
// Everywhere except Linux these are thin pass-throughs to file_picker.
// On Linux they are NOT, and the reason is worth spelling out because
// the failure it works around is completely silent.
//
// file_picker has no native Linux dialog — it shells out to whichever of
// zenity / qarma / kdialog / matedialog it finds on PATH and scrapes
// stdout. That is fine for a distro build, but the player also ships as
// an AppImage, and an AppImage's AppRun exports LD_LIBRARY_PATH pointing
// at the bundled GTK/GLib stack. A spawned child inherits it, so the
// HOST's /usr/bin/zenity starts up against OUR bundled libraries, hits
// an ABI mismatch (host zenity is GTK4 on current distros, the bundle is
// the GTK3 stack Flutter links), and dies before it can draw:
//
//     zenity: error while loading shared libraries: libgio-2.0.so.0:
//             file too short                          [exit code 127]
//
// Nothing reaches stdout, so file_picker reports "user cancelled" and
// every picker button in the app looks completely dead.
//
// So on Linux we run the dialog ourselves with a sanitized environment:
// the AppImage-injected loader/module variables are stripped so the host
// binary loads host libraries, exactly as it would from a normal shell.
// We also distinguish "user pressed Cancel" from "no dialog binary is
// installed" and "the dialog crashed", because the last two need to be
// reported rather than swallowed.

import 'dart:io';
import 'dart:typed_data';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/foundation.dart' show visibleForTesting;

/// Outcome of a dialog attempt.
enum PickStatus {
  /// The user made a choice; `path` is non-null.
  picked,

  /// The user dismissed the dialog. Not an error — say nothing.
  cancelled,

  /// No usable dialog program is installed (Linux only). Callers should
  /// surface `message`; otherwise the button looks broken.
  unavailable,

  /// A dialog program was found but failed to run (Linux only).
  failed,
}

/// Result of [pickDirectory] and [saveFile].
class PickResult {
  const PickResult(this.status, {this.path, this.message});

  final PickStatus status;
  final String? path;
  final String? message;

  bool get ok => status == PickStatus.picked;

  /// True when the caller should show [message] to the user.
  bool get isError =>
      status == PickStatus.unavailable || status == PickStatus.failed;

  static const cancelled = PickResult(PickStatus.cancelled);
}

/// Result of [pickFile]. Carries bytes so callers that upload don't have
/// to care whether the platform handed back a path or a sandboxed blob.
class FileResult {
  const FileResult(this.status, {this.path, this.bytes, this.message});

  final PickStatus status;
  final String? path;
  final Uint8List? bytes;
  final String? message;

  bool get ok => status == PickStatus.picked;
  bool get isError =>
      status == PickStatus.unavailable || status == PickStatus.failed;

  /// Basename of the chosen file, or empty when nothing was chosen.
  String get name {
    final p = path;
    if (p == null || p.isEmpty) return '';
    return p.split(Platform.pathSeparator).last;
  }

  static const cancelled = FileResult(PickStatus.cancelled);
}

// ---------------------------------------------------------------------
// Linux environment sanitation
// ---------------------------------------------------------------------

/// Variables an AppImage's AppRun injects that must NOT leak into a
/// spawned host binary. LD_* redirect the dynamic loader; the GTK/GDK/GIO
/// entries point at module caches describing only the bundled libraries.
const _appImageEnvVars = <String>{
  'LD_LIBRARY_PATH',
  'LD_PRELOAD',
  'GTK_PATH',
  'GTK_EXE_PREFIX',
  'GTK_DATA_PREFIX',
  'GTK_IM_MODULE_FILE',
  'GDK_PIXBUF_MODULE_FILE',
  'GDK_PIXBUF_MODULEDIR',
  'GSETTINGS_SCHEMA_DIR',
  'GIO_MODULE_DIR',
  'GI_TYPELIB_PATH',
  'FONTCONFIG_FILE',
  'FONTCONFIG_PATH',
  'PYTHONHOME',
};

/// Build the environment a host dialog binary should see: our own env
/// minus [_appImageEnvVars], with PATH/LD_LIBRARY_PATH restored to the
/// values AppRun snapshotted before it prepended the AppDir (see
/// scripts/build-linux-appimage.sh). Outside an AppImage the backup vars
/// are absent and this is very nearly a straight copy.
Map<String, String> _hostEnvironment([Map<String, String>? source]) {
  final src = source ?? Platform.environment;
  final env = <String, String>{};
  src.forEach((k, v) {
    if (_appImageEnvVars.contains(k)) return;
    env[k] = v;
  });

  final hostPath = src['BOPWIRE_HOST_PATH'];
  if (hostPath != null && hostPath.isNotEmpty) env['PATH'] = hostPath;

  final hostLd = src['BOPWIRE_HOST_LD_LIBRARY_PATH'];
  if (hostLd != null && hostLd.isNotEmpty) env['LD_LIBRARY_PATH'] = hostLd;

  // Our own bookkeeping and the AppImage runtime's — no reason to pass
  // any of it to a host dialog.
  env.remove('BOPWIRE_HOST_PATH');
  env.remove('BOPWIRE_HOST_LD_LIBRARY_PATH');
  env.remove('APPDIR');
  env.remove('APPIMAGE');
  env.remove('ARGV0');
  return env;
}

/// Resolve [binary] against [env]'s PATH without invoking `which`, which
/// is itself a host binary we would rather not depend on.
String? _resolveOnPath(String binary, Map<String, String> env) {
  final path = env['PATH'];
  if (path == null || path.isEmpty) return null;
  for (final dir in path.split(':')) {
    if (dir.isEmpty) continue;
    final candidate = File('$dir/$binary');
    if (!candidate.existsSync()) continue;
    // 0x49 == 0o111: any of user/group/other execute.
    if (candidate.statSync().mode & 0x49 != 0) return candidate.path;
  }
  return null;
}

/// The dialog program to drive, or null if none is installed.
({String path, String name})? _findDialog(Map<String, String> env) {
  final desktop = (env['XDG_CURRENT_DESKTOP'] ?? '').toLowerCase();
  // Prefer the toolkit-native chooser for the running desktop so the
  // dialog matches the rest of the session, then fall back.
  final candidates = desktop.contains('kde')
      ? const ['kdialog', 'zenity', 'qarma', 'matedialog']
      : const ['zenity', 'qarma', 'kdialog', 'matedialog'];
  for (final name in candidates) {
    final resolved = _resolveOnPath(name, env);
    if (resolved != null) return (path: resolved, name: name);
  }
  return null;
}

const _noDialogMessage =
    'No file-picker dialog is installed. Install "zenity" (GNOME and '
    'most desktops) or "kdialog" (KDE) with your package manager, then '
    'try again.';

/// Run a dialog and return the selected path, or a non-picked status.
Future<PickResult> _runDialog(List<String> Function(String name) buildArgs) async {
  final env = _hostEnvironment();
  final dialog = _findDialog(env);
  if (dialog == null) {
    return const PickResult(PickStatus.unavailable, message: _noDialogMessage);
  }

  ProcessResult res;
  try {
    res = await Process.run(
      dialog.path,
      buildArgs(dialog.name),
      environment: env,
      includeParentEnvironment: false,
      stdoutEncoding: systemEncoding,
      stderrEncoding: systemEncoding,
    );
  } on ProcessException catch (e) {
    return PickResult(
      PickStatus.failed,
      message: 'Could not open the file picker (${dialog.name}): ${e.message}',
    );
  }

  final out = (res.stdout as String).trim();
  if (res.exitCode == 0 && out.isNotEmpty) {
    // zenity emits a '|'-separated list when multi-select is on; we
    // never enable it, so take the first entry defensively.
    return PickResult(PickStatus.picked, path: out.split('|').first.trim());
  }

  // Exit 1 with no output is the documented "user cancelled" for both
  // zenity and kdialog. Anything else is a real failure — most often the
  // library mismatch this file exists to avoid — so report it rather
  // than pretending the user cancelled.
  if (res.exitCode == 1 && out.isEmpty) return PickResult.cancelled;

  final err = (res.stderr as String).trim();
  if (err.isEmpty) return PickResult.cancelled;
  return PickResult(
    PickStatus.failed,
    message: 'The file picker (${dialog.name}) failed: ${err.split('\n').first}',
  );
}

/// zenity wants `--file-filter='Label | *.a *.b'`; kdialog wants a
/// positional `'*.a *.b|Label'`.
String _zenityFilter(String label, List<String> extensions) =>
    '$label | ${extensions.map((e) => '*.$e').join(' ')}';

String _kdialogFilter(String label, List<String> extensions) =>
    '${extensions.map((e) => '*.$e').join(' ')}|$label';

// ---------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------

/// Ask the user to choose a directory.
Future<PickResult> pickDirectory({
  String title = 'Select a folder',
  String? initialDir,
}) async {
  if (!Platform.isLinux) {
    final path = await FilePicker.platform.getDirectoryPath(
      dialogTitle: title,
      initialDirectory: initialDir,
    );
    return path == null
        ? PickResult.cancelled
        : PickResult(PickStatus.picked, path: path);
  }

  return _runDialog((name) {
    if (name == 'kdialog') {
      return ['--title', title, '--getexistingdirectory', initialDir ?? '.'];
    }
    final start = initialDir == null
        ? null
        : (initialDir.endsWith('/') ? initialDir : '$initialDir/');
    return [
      '--file-selection',
      '--directory',
      '--title=$title',
      if (start != null) '--filename=$start',
    ];
  });
}

/// Ask the user to choose an existing file, returning its bytes.
///
/// [extensions] are bare, without a dot ('pdf', 'png'). [filterLabel] is
/// what the filter row is called in the dialog.
Future<FileResult> pickFile({
  String title = 'Choose a file',
  List<String> extensions = const [],
  String filterLabel = 'Supported files',
  int? maxBytes,
}) async {
  if (!Platform.isLinux) {
    final picked = await FilePicker.platform.pickFiles(
      dialogTitle: title,
      type: extensions.isEmpty ? FileType.any : FileType.custom,
      allowedExtensions: extensions.isEmpty ? null : extensions,
      withData: true,
    );
    if (picked == null || picked.files.isEmpty) return FileResult.cancelled;
    final f = picked.files.single;
    var bytes = f.bytes;
    if (bytes == null && f.path != null) {
      bytes = await File(f.path!).readAsBytes();
    }
    if (bytes == null) {
      return const FileResult(PickStatus.failed,
          message: 'Could not read the selected file.');
    }
    return FileResult(PickStatus.picked, path: f.path, bytes: bytes);
  }

  final res = await _runDialog((name) {
    if (name == 'kdialog') {
      return [
        '--title', title,
        '--getopenfilename', '.',
        if (extensions.isNotEmpty) _kdialogFilter(filterLabel, extensions),
      ];
    }
    return [
      '--file-selection',
      '--title=$title',
      if (extensions.isNotEmpty)
        '--file-filter=${_zenityFilter(filterLabel, extensions)}',
    ];
  });

  if (!res.ok) {
    return FileResult(res.status, message: res.message);
  }

  final file = File(res.path!);
  if (!file.existsSync()) {
    return FileResult(PickStatus.failed,
        message: 'The selected file no longer exists: ${res.path}');
  }
  if (maxBytes != null && file.lengthSync() > maxBytes) {
    // Let the caller's own size check produce the user-facing wording;
    // we still return the path so it can report the real size.
    return FileResult(PickStatus.picked, path: res.path, bytes: null);
  }
  try {
    return FileResult(PickStatus.picked,
        path: res.path, bytes: await file.readAsBytes());
  } on FileSystemException catch (e) {
    return FileResult(PickStatus.failed,
        message: 'Could not read ${res.path}: ${e.message}');
  }
}

/// Ask the user where to save a file.
///
/// On mobile the system picker performs the write itself, which is why
/// [bytes] is forwarded to file_picker. On Linux (and desktop generally)
/// the dialog only returns a path and the caller does the writing.
Future<PickResult> saveFile({
  String title = 'Save file',
  required String fileName,
  Uint8List? bytes,
}) async {
  if (!Platform.isLinux) {
    final path = await FilePicker.platform.saveFile(
      dialogTitle: title,
      fileName: fileName,
      bytes: bytes,
    );
    return path == null
        ? PickResult.cancelled
        : PickResult(PickStatus.picked, path: path);
  }

  return _runDialog((name) {
    if (name == 'kdialog') {
      return ['--title', title, '--getsavefilename', './$fileName'];
    }
    return [
      '--file-selection',
      '--save',
      '--title=$title',
      '--filename=$fileName',
    ];
  });
}


// ---------------------------------------------------------------------
// Test seams
//
// The Linux path is the whole point of this file, and its two failure
// modes (no dialog installed; AppImage env leaking into the child) are
// exactly the ones that used to fail silently. Both are pure functions
// over an environment map, so they are unit-tested directly rather than
// by trying to spawn dialogs in CI.
// ---------------------------------------------------------------------

@visibleForTesting
Map<String, String> sanitizeEnvironmentForTest(Map<String, String> source) =>
    _hostEnvironment(source);

@visibleForTesting
String? resolveDialogForTest(Map<String, String> env) =>
    _findDialog(env)?.name;

@visibleForTesting
const String noDialogMessageForTest = _noDialogMessage;
