#!/usr/bin/env ruby

if __FILE__ == $PROGRAM_NAME
  require 'fileutils'
  FileUtils.mkdir_p 'tmp'
  unless File.exist?('tmp/mruby')
    system 'git clone https://github.com/mruby/mruby.git tmp/mruby'
  end
  exit system(%Q[cd tmp/mruby; MRUBY_CONFIG=#{File.expand_path __FILE__} ./minirake #{ARGV.join(' ')}])
end

MRuby::Lockfile.disable rescue nil # for development

MRuby::Build.new do |conf|
  toolchain
  #conf.enable_debug
  conf.enable_test
  conf.enable_bintest
  #conf.disable_presym rescue nil

  unless ENV['VisualStudioVersion'] || ENV['VSINSTALLDIR']
    conf.cc.flags << ["-fPIC"]
    conf.cxx.flags << ["-fPIC"]
  end
  archiver.command = cc.command
  archiver.archive_options = "-shared -o %{outfile} %{objs}"
  conf.exts.library = ".so"

  conf.gem core: 'mruby-print'
  conf.gem __dir__
  conf.gembox 'default'
end
