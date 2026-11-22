// The AsyncAPI documents committed under spec/asyncapi/ through the official AsyncAPI parser,
// @asyncapi/parser (github.com/asyncapi/parser-js), run by Node.js. CTest runs it only when the
// cache variable PRAENORA_ASYNCAPI_PARSER names a directory into which that parser is installed;
// neither the build nor the tests download it (docs/testing.md says which version was recorded and
// how to install it). The parser runs with its default rules, and reads no file and no URL: it is
// given each document's text, and the documents refer only to themselves.
//
// Usage: node asyncapi_parser_check.cjs <check> <parser directory> <AsyncAPI directory>
//   <check>               accepts_committed_documents or refuses_broken_copies
//   <parser directory>    the directory that holds node_modules/@asyncapi/parser
//   <AsyncAPI directory>  the directory of the committed documents, spec/asyncapi
// It exits with 0 when the check passes, 1 when it fails and 2 when it is called wrongly.
'use strict';

const fs = require('fs');
const path = require('path');

// The version of the parser that these checks were recorded with. Another version may report
// other diagnostics: to move to it, install it, change this line and record in
// RECORDED_DIAGNOSTICS what it reports; if refuses_broken_copies then fails, check each break's
// expected code and place against the new version's rules, without dropping a break.
const PARSER_VERSION = '3.6.6';

// What the parser reports for every committed document, and all it may report: a document that
// reports a diagnostic not listed here, or does not report one that is, fails. None of them may be
// an error.
// asyncapi-latest-version says that the latest AsyncAPI version is 3.1.0, while the documents are
// AsyncAPI 3.0.0 (ADR-0020).
const RECORDED_DIAGNOSTICS = [
    { severity: 'info', code: 'asyncapi-latest-version', pointer: '/asyncapi' },
];

// The document that the broken copies are made from.
const BROKEN_COPIES_OF = 'example.estimate-predict-0.1.0.asyncapi.json';

// Copies of that document, each broken in one place, and the error that the parser reports for
// each: its code (the rule's name, or invalid-ref for a reference that does not resolve) and
// where. All but the last break a rule of the AsyncAPI specification that the AsyncAPI 3.0.0 JSON
// Schema does not express; the last breaks the schema, which the parser applies too.
const BREAKS = [
    {
        what: 'an operation whose message belongs to another channel',
        change(document) {
            document.operations['estimate.state.publish'].messages = [
                { $ref: '#/channels/predict.predictions/messages/pmp.prediction-set' },
            ];
        },
        code: 'asyncapi3-operation-messages-from-referred-channel',
        pointer: '/operations/estimate.state.publish/messages/0',
    },
    {
        what: 'an operation whose channel is not under the root channels',
        change(document) {
            document.components.channels = { elsewhere: document.channels['estimate.state'] };
            document.operations['estimate.state.publish'].channel = {
                $ref: '#/components/channels/elsewhere',
            };
        },
        code: 'asyncapi3-required-operation-channel-unambiguity',
        pointer: '/operations/estimate.state.publish/channel/$ref',
    },
    {
        what: 'a channel whose server is not under the root servers',
        change(document) {
            document.components.servers = { local: { host: 'localhost', protocol: 'ws' } };
            document.channels['estimate.state'].servers = [
                { $ref: '#/components/servers/local' },
            ];
        },
        code: 'asyncapi3-required-channel-servers-unambiguity',
        pointer: '/channels/estimate.state/servers/0/$ref',
    },
    {
        what: 'a channel key that holds a query delimiter',
        change(document) {
            document.channels['estimate.state?tick=0'] = {
                address: 'example.estimate-predict/estimate.state',
            };
        },
        code: 'asyncapi3-channel-no-query-nor-fragment',
        pointer: '/channels/estimate.state?tick=0',
    },
    {
        what: 'a channel parameter that its address does not use',
        change(document) {
            document.channels['estimate.state'].parameters = { step: {} };
        },
        code: 'asyncapi3-channel-parameters',
        pointer: '/channels/estimate.state/parameters/step',
    },
    {
        what: 'a reference that does not resolve',
        change(document) {
            document.operations['estimate.state.publish'].channel = { $ref: '#/channels/nowhere' };
        },
        code: 'invalid-ref',
        pointer: '/operations/estimate.state.publish/channel/$ref',
    },
    {
        what: 'an info without its version (the AsyncAPI 3.0.0 JSON Schema)',
        change(document) {
            delete document.info.version;
        },
        code: 'asyncapi-document-resolved',
        pointer: '/info',
    },
];

// Spectral's severities, by number.
const SEVERITIES = ['error', 'warning', 'info', 'hint'];

class CheckFailure extends Error {}

class UsageError extends Error {}

function readText(file) {
    return fs.readFileSync(file, 'utf8');
}

// The JSON pointer of a diagnostic's path.
function pointerOf(segments) {
    return segments
        .map((segment) => '/' + String(segment).replace(/~/g, '~0').replace(/\//g, '~1'))
        .join('');
}

function keyOf(diagnostic) {
    return `${diagnostic.severity} ${diagnostic.code} at ${diagnostic.pointer || '(root)'}`;
}

function described(diagnostics) {
    if (diagnostics.length === 0) {
        return '  (none)\n';
    }
    return diagnostics
        .map((d) => `  ${keyOf(d)}${d.message === undefined ? '' : ': ' + d.message}\n`)
        .join('');
}

// The version of the package `name` that the parser in `parserDirectory` loads, or 'not found'.
// These versions are only reported. Its package.json is looked for where Node.js looks for the
// package, in the node_modules of the parser's real directory (symbolic links resolved) and of
// each directory above it; it is read directly, since a package's "exports" can keep
// require.resolve() from reaching it.
function dependencyVersion(name, parserDirectory) {
    for (let directory = fs.realpathSync(parserDirectory); ; directory = path.dirname(directory)) {
        const manifest = path.join(directory, 'node_modules', ...name.split('/'), 'package.json');
        if (fs.existsSync(manifest)) {
            return JSON.parse(readText(manifest)).version;
        }
        if (path.dirname(directory) === directory) {
            return 'not found';
        }
    }
}

// The parser installed in `directory`, after checking its version.
function loadParser(directory) {
    // An absolute path: require() reads a relative one as a package name, or against this
    // script's directory.
    const parserDirectory = path.join(
        path.resolve(directory),
        'node_modules',
        '@asyncapi',
        'parser',
    );
    const manifest = path.join(parserDirectory, 'package.json');
    if (!fs.existsSync(manifest)) {
        throw new UsageError(`no @asyncapi/parser in ${directory}: ${manifest} does not exist`);
    }
    const version = JSON.parse(readText(manifest)).version;
    const specs = dependencyVersion('@asyncapi/specs', parserDirectory);
    const spectral = dependencyVersion('@stoplight/spectral-core', parserDirectory);
    process.stdout.write(
        `@asyncapi/parser ${version} (with @asyncapi/specs ${specs} and ` +
            `@stoplight/spectral-core ${spectral}) at ${parserDirectory}, ` +
            `Node.js ${process.version}\n`,
    );
    if (version !== PARSER_VERSION) {
        throw new CheckFailure(
            `@asyncapi/parser is ${version}, but these checks were recorded with ` +
                `${PARSER_VERSION}: install ${PARSER_VERSION} (docs/testing.md), or move ` +
                'PARSER_VERSION and RECORDED_DIAGNOSTICS in this script to the new version',
        );
    }
    const { Parser } = require(parserDirectory);
    return new Parser();
}

// What the parser makes of a document's text: the document it parsed (undefined when it reported
// an error) and its diagnostics.
async function parsed(parser, text) {
    const output = await parser.parse(text);
    const diagnostics = output.diagnostics.map((d) => ({
        severity: SEVERITIES[d.severity] || `severity ${d.severity}`,
        code: String(d.code),
        pointer: pointerOf(d.path),
        message: d.message,
    }));
    diagnostics.sort((a, b) => (keyOf(a) < keyOf(b) ? -1 : keyOf(a) > keyOf(b) ? 1 : 0));
    return { document: output.document, diagnostics };
}

// The committed documents, in name order.
function committedDocuments(directory) {
    const names = fs
        .readdirSync(directory)
        .filter((name) => name.endsWith('.asyncapi.json'))
        .sort();
    if (names.length === 0) {
        throw new CheckFailure(`no *.asyncapi.json in ${directory}`);
    }
    return names;
}

// Every committed document parses, and reports exactly the recorded diagnostics.
async function acceptsCommittedDocuments(parser, directory) {
    let failures = 0;
    for (const name of committedDocuments(directory)) {
        const text = readText(path.join(directory, name));
        const { document, diagnostics } = await parsed(parser, text);
        const unrecorded = diagnostics.slice();
        const missing = [];
        for (const recorded of RECORDED_DIAGNOSTICS) {
            const index = unrecorded.findIndex((d) => keyOf(d) === keyOf(recorded));
            if (index < 0) {
                missing.push(recorded);
            } else {
                unrecorded.splice(index, 1);
            }
        }
        if (document !== undefined && unrecorded.length === 0 && missing.length === 0) {
            process.stdout.write(`PASS ${name}: parsed, with the recorded diagnostics:\n`);
            process.stdout.write(described(diagnostics));
            continue;
        }
        failures += 1;
        process.stdout.write(
            `FAIL ${name}: ` +
                (document === undefined ? 'not parsed' : 'parsed') +
                ', with other diagnostics than the recorded ones\n' +
                'not recorded:\n' +
                described(unrecorded) +
                'recorded, not reported:\n' +
                described(missing),
        );
    }
    if (failures > 0) {
        throw new CheckFailure(`${failures} committed document(s) failed`);
    }
}

// Each broken copy of a committed document is refused, with the error that names its break.
async function refusesBrokenCopies(parser, directory) {
    const source = path.join(directory, BROKEN_COPIES_OF);
    if (!fs.existsSync(source)) {
        throw new CheckFailure(`${source}, which the broken copies are made from, does not exist`);
    }
    const text = readText(source);
    const original = await parsed(parser, text);
    const originalErrors = original.diagnostics.filter((d) => d.severity === 'error');
    if (original.document === undefined || originalErrors.length > 0) {
        throw new CheckFailure(
            `${BROKEN_COPIES_OF} itself is not parsed without errors:\n` +
                described(original.diagnostics),
        );
    }
    let failures = 0;
    for (const item of BREAKS) {
        const copy = JSON.parse(text);
        item.change(copy);
        const { document, diagnostics } = await parsed(parser, JSON.stringify(copy, null, 2));
        const expected = { severity: 'error', code: item.code, pointer: item.pointer };
        const found = diagnostics.some((d) => keyOf(d) === keyOf(expected));
        if (document === undefined && found) {
            process.stdout.write(`PASS ${item.what}: refused with ${keyOf(expected)}\n`);
            continue;
        }
        failures += 1;
        process.stdout.write(
            `FAIL ${item.what}: ` +
                (document === undefined ? 'refused' : 'parsed') +
                `, expected ${keyOf(expected)}; the parser reported:\n` +
                described(diagnostics),
        );
    }
    if (failures > 0) {
        throw new CheckFailure(`${failures} of ${BREAKS.length} broken copies were not refused`);
    }
}

const CHECKS = {
    accepts_committed_documents: acceptsCommittedDocuments,
    refuses_broken_copies: refusesBrokenCopies,
};

async function main(args) {
    if (args.length !== 3 || !Object.prototype.hasOwnProperty.call(CHECKS, args[0])) {
        throw new UsageError(
            'usage: node asyncapi_parser_check.cjs ' +
                `(${Object.keys(CHECKS).join(' | ')}) <parser directory> <AsyncAPI directory>`,
        );
    }
    const [check, parserDirectory, directory] = args;
    const parser = loadParser(parserDirectory);
    await CHECKS[check](parser, path.resolve(directory));
    process.stdout.write(`${check}: passed\n`);
}

main(process.argv.slice(2)).then(
    () => {
        process.exitCode = 0;
    },
    (error) => {
        if (error instanceof CheckFailure) {
            process.stdout.write(`${error.message}\n`);
            process.exitCode = 1;
        } else if (error instanceof UsageError) {
            process.stderr.write(`${error.message}\n`);
            process.exitCode = 2;
        } else {
            process.stderr.write(`${error && error.stack ? error.stack : error}\n`);
            process.exitCode = 1;
        }
    },
);
