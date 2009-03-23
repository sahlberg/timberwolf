"""
Makefile execution.

Multiple `makes` can be run within the same process. Each one has an entirely data.Makefile and .Target
structure, environment, and working directory. Typically they will all share a parallel execution context,
except when a submake specifies -j1 when the parent make is building in parallel.
"""

import os, subprocess, sys, logging, time, traceback
from optparse import OptionParser
import data, parserdata, process, util

# TODO: If this ever goes from relocatable package to system-installed, this may need to be
# a configured-in path.

makepypath = os.path.normpath(os.path.join(os.path.dirname(__file__), '../make.py'))

def parsemakeflags(env):
    """
    Parse MAKEFLAGS from the environment into a sequence of command-line arguments.
    """

    makeflags = env.get('MAKEFLAGS', '')
    makeflags = makeflags.strip()

    if makeflags == '':
        return []

    if makeflags[0] not in ('-', ' '):
        makeflags = '-' + makeflags

    opts = []
    curopt = ''

    i = 0
    while i < len(makeflags):
        c = makeflags[i]
        if c.isspace():
            opts.append(curopt)
            curopt = ''
            i += 1
            while i < len(makeflags) and makeflags[i].isspace():
                i += 1
            continue

        if c == '\\':
            i += 1
            if i == len(makeflags):
                raise data.DataError("MAKEFLAGS has trailing backslash")
            c = makeflags[i]
            
        curopt += c
        i += 1

    if curopt != '':
        opts.append(curopt)

    return opts

def _version(*args):
    print """pymake: GNU-compatible make program
Copyright (C) 2009 The Mozilla Foundation <http://www.mozilla.org/>
This is free software; see the source for copying conditions.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE."""

_log = logging.getLogger('pymake.execution')

def main(args, env, cwd, context, cb):
    """
    Start a single makefile execution, given a command line, working directory, and environment.

    @param cb a callback to notify with an exit code when make execution is finished.
    """

    try:
        makelevel = int(env.get('MAKELEVEL', '0'))

        op = OptionParser()
        op.add_option('-f', '--file', '--makefile',
                      action='append',
                      dest='makefiles',
                      default=[])
        op.add_option('-d',
                      action="store_true",
                      dest="verbose", default=False)
        op.add_option('--debug-log',
                      dest="debuglog", default=None)
        op.add_option('-C', '--directory',
                      dest="directory", default=None)
        op.add_option('-v', '--version', action="store_true",
                      dest="printversion", default=False)
        op.add_option('-j', '--jobs', type="int",
                      dest="jobcount", default=1)
        op.add_option('--no-print-directory', action="store_false",
                      dest="printdir", default=True)

        options, arguments1 = op.parse_args(parsemakeflags(env))
        options, arguments2 = op.parse_args(args, values=options)

        arguments = arguments1 + arguments2

        if options.printversion:
            _version()
            cb(0)
            return

        shortflags = []
        longflags = []

        loglevel = logging.WARNING
        if options.verbose:
            loglevel = logging.DEBUG
            shortflags.append('d')

        logkwargs = {}
        if options.debuglog:
            logkwargs['filename'] = options.debuglog
            longflags.append('--debug-log=%s' % options.debuglog)

        if options.directory is None:
            workdir = cwd
        else:
            workdir = os.path.join(cwd, options.directory)

        shortflags.append('j%i' % (options.jobcount,))

        makeflags = ''.join(shortflags) + ' ' + ' '.join(longflags)

        logging.basicConfig(level=loglevel, **logkwargs)

        if context is not None and context.jcount > 1 and options.jobcount == 1:
            _log.debug("-j1 specified, creating new serial execution context")
            context = process.getcontext(options.jobcount)
            subcontext = True
        elif context is None:
            _log.debug("Creating new execution context, jobcount %s", options.jobcount)
            context = process.getcontext(options.jobcount)
            subcontext = True
        else:
            _log.debug("Using parent execution context")
            subcontext = False

        if options.printdir:
            print "make.py[%i]: Entering directory '%s'" % (makelevel, workdir)
            sys.stdout.flush()

        if len(options.makefiles) == 0:
            if os.path.exists(os.path.join(workdir, 'Makefile')):
                options.makefiles.append('Makefile')
            else:
                print "No makefile found"
                cb(2)
                return

        overrides, targets = parserdata.parsecommandlineargs(arguments)

        def makecb(error, didanything, makefile, realtargets, tstack, i, firsterror):
            if error is not None:
                print error
                if firsterror is None:
                    firsterror = error

            if i == len(realtargets):
                if subcontext:
                    context.finish()

                if options.printdir:
                    print "make.py[%i]: Leaving directory '%s'" % (makelevel, workdir)
                sys.stdout.flush()

                context.defer(cb, firsterror and 2 or 0)
            else:
                deferredmake = process.makedeferrable(makecb, makefile=makefile,
                                                      realtargets=realtargets, tstack=tstack, i=i+1, firsterror=firsterror)

                makefile.gettarget(realtargets[i]).make(makefile, tstack, [], cb=deferredmake)
                                                                                  

        def remakecb(remade, restarts, makefile):
            if remade:
                if restarts > 0:
                    _log.info("make.py[%i]: Restarting makefile parsing", makelevel)
                makefile = data.Makefile(restarts=restarts, make='%s %s' % (sys.executable.replace('\\', '/'), makepypath.replace('\\', '/')),
                                         makeflags=makeflags, makelevel=makelevel, workdir=workdir,
                                         context=context, env=env)

                try:
                    overrides.execute(makefile)
                    for f in options.makefiles:
                        makefile.include(f)
                    makefile.finishparsing()
                    makefile.remakemakefiles(process.makedeferrable(remakecb, restarts=restarts + 1, makefile=makefile))

                except util.MakeError, e:
                    print e
                    context.defer(cb, 2)
                    return

                return

            if len(targets) == 0:
                if makefile.defaulttarget is None:
                    print "No target specified and no default target found."
                    context.defer(cb, 2)
                    return

                _log.info("Making default target %s", makefile.defaulttarget)
                realtargets = [makefile.defaulttarget]
                tstack = ['<default-target>']
            else:
                realtargets = targets
                tstack = ['<command-line>']

            deferredmake = process.makedeferrable(makecb, makefile=makefile,
                                                  realtargets=realtargets, tstack=tstack, i=1, firsterror=None)
            makefile.gettarget(realtargets[0]).make(makefile, tstack, [], cb=deferredmake)

        context.defer(remakecb, True, 0, None)

    except (util.MakeError), e:
        print e
        if options.printdir:
            print "make.py[%i]: Leaving directory '%s'" % (makelevel, workdir)
        sys.stdout.flush()
        cb(2)
        return