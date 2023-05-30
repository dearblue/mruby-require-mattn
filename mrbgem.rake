require "mruby/source"

module MRuby
  module Gem
    class List
      include Enumerable
      def reject!(&x)
        @ary.reject! &x
      end
      def uniq(&x)
        @ary.uniq &x
      end
    end
  end
  class Build
    unless method_defined?(:old_print_build_summary_for_require)
      alias_method :old_print_build_summary_for_require, :print_build_summary
    end
    def print_build_summary
      old_print_build_summary_for_require

      Rake::Task.tasks.each do |t|
        if t.name =~ /\.so$/
          t.invoke
        end
      end

      unless !defined?(@bundled) || @bundled.empty?
        puts "================================================"
        puts "     Bundled Gems:"
        @bundled.sort_by(&:name).each do |g|
          print "             #{g.name}"
          print " - #{g.version}" if g.version && MRuby::Gem::Version.new(g.version) > [0]
          print " - #{g.summary}" if g.summary && !g.summary.empty?
          puts
        end
        puts "================================================"
        puts
      end
    end
  end
end

module MRubyRequire
  refine Array do
    # Move those in white_list or on which they depend before sentinel.
    def modify_order_by_white_list(sentinel, white_list)
      moves = white_list.map { |e| self.find_index { |g| g.name == e } }
      moves.compact!
      traverse_dependencies = ->(a, off) {
        if g = self[off]
          a << off
          g.dependencies.each do |d|
            traverse_dependencies.call a, self.find_index { |gg| gg.name == d[:gem] }
          end
        end
      }
      moves = moves.each_with_object([]) { |e, a| traverse_dependencies.call(a, e) }
      moves.reject! { |e| e <= sentinel }
      moves.uniq!
      moves.sort!
      moves.reverse!
      moves.map! { |e| self.delete_at e }
      self[sentinel, 0] = moves
      sentinel + moves.size
    end
  end
end

using MRubyRequire

MRuby::Gem::Specification.new('mruby-require') do |spec|
  spec.license = 'MIT'
  spec.authors = 'mattn'
  spec.add_test_dependency 'mruby-bin-mruby', core: 'mruby-bin-mruby'
  spec.add_test_dependency 'mruby-metaprog', core: 'mruby-metaprog'

  ENV["MRUBY_REQUIRE"] = ""

  is_vc = ENV['OS'] == 'Windows_NT' && cc.command =~ /^cl(\.exe)?$/
  is_mingw = ENV['OS'] == 'Windows_NT' && cc.command =~ /^(?:clang|gcc)(.*\.exe)?$/
  top_build_dir = build.build_dir

  if MRuby::Source::MRUBY_RELEASE_NO < 30000
    testlib = libfile("#{build.build_dir}/mrbgems/mruby-test/mrbtest")
  end

  intercept = "mruby-require:trap#{rand 1 << 30}:#{build.name}"
  file build.libmruby_core_static => intercept
  file "#{build.build_dir}/mrbgems/gem_init.c" => intercept
  task intercept => __FILE__ do |t|
    Rake::Task[build.libmruby_core_static].prerequisites.delete intercept
    class << t; def timestamp; Time.at(0); end; end

    next unless build.enable_gems?
    gems = build.gems
    # Only gems included AFTER the mruby-require gem during compilation are
    # compiled as separate objects.  However, gems included in white_list
    # and the gems on which they depend will be incorporated into libmruby.
    gems_uniq   = gems.uniq {|x| x.name}
    white_list  = ["mruby-require", "mruby-test", "mruby-bin-mrbc", "mruby-complex", "mruby-rational", "mruby-bigint"]
    mr_position = gems_uniq.find_index {|g| g.name == "mruby-require" }
    mr_position = gems_uniq.modify_order_by_white_list(mr_position, white_list)
    gems.instance_eval { @ary.replace gems_uniq }
    compiled_in = gems_uniq[0..mr_position].map {|g| g.name}
    bundled = gems_uniq.reject {|g| compiled_in.include?(g.name)}
    gems.reject! {|g| !compiled_in.include?(g.name) and !white_list.include?(g.name)}
    libmruby_libs      = build.linker.libraries
    libmruby_lib_paths = build.linker.library_paths
    gems.each { |e| bundled.delete e }

    libmruby_objs = Rake::Task[build.libmruby_static].prerequisites
    bundled.each do |g|
      next if g.objs.nil? or g.objs.empty?
      g.objs.each { |e| libmruby_objs.delete e }
      ENV["MRUBY_REQUIRE"] += "#{g.name},"
      sharedlib = "#{top_build_dir}/lib/#{g.name}.so"
      file sharedlib => g.objs do |t|
        if RUBY_PLATFORM.downcase =~ /mswin(?!ce)|mingw|bccwin/
          libmruby_libs += %w(msvcrt kernel32 user32 gdi32 winspool comdlg32)
          name = g.name.gsub(/-/, '_')
          deffile = "#{build.build_dir}/lib/#{g.name}.def"
          open(deffile, 'w') do |f|
            f.puts %Q[EXPORTS]
            if g.generate_functions
              f.puts %Q[	GENERATED_TMP_mrb_#{name}_gem_init]
              f.puts %Q[	GENERATED_TMP_mrb_#{name}_gem_final]
            end
          end
        else
          deffile = ''
        end
        options = {
            :flags => [
                is_vc ? '/DLL' : is_mingw ? '-shared' : '-shared -fPIC',
                (libmruby_lib_paths + (g.linker ? g.linker.library_paths : [])).flatten.map {|l| is_vc ? "/LIBPATH:#{l}" : "-L#{l}"}].flatten.join(" "),
            :outfile => sharedlib,
            :objs => g.objs.flatten.join(" "),
            :libs => [
                (is_vc ? '/DEF:' : '') + deffile,
                build.libfile("#{build.build_dir}/lib/libmruby"),
                (libmruby_libs + (g.linker ? g.linker.libraries : [])).flatten.uniq.map {|l| is_vc ? "#{l}.lib" : "-l#{l}"}].flatten.join(" "),
            :flags_before_libraries => g.linker ? g.linker.flags_before_libraries.flatten.join(" ") : '',
            :flags_after_libraries => '',
        }

        _pp "LD", sharedlib
        sh build.linker.command + ' ' + (build.linker.link_options % options)
      end

      file sharedlib => build.libfile("#{build.build_dir}/lib/libmruby")
      Rake::Task.tasks << sharedlib
      file testlib => sharedlib if testlib
    end
    build.libmruby.flatten!.reject! do |l|
      bundled.reject {|g| l.index(g.name) == nil}.size > 0
    end
    build.cc.include_paths.reject! do |l|
      bundled.reject {|g| l.index(g.name) == nil}.size > 0
    end
    bundled = build.instance_eval { @bundled = bundled }
  end

  if testlib
    file testlib => intercept do |t|
      t.prerequisites.delete intercept
    end
  end

  unless spec.cc.flags.flatten.find {|e| e.match /DMRBGEMS_ROOT/}
    if RUBY_PLATFORM.downcase !~ /mswin(?!ce)|mingw|bccwin/
      spec.linker.libraries << ['dl']
      spec.cc.flags << "-DMRBGEMS_ROOT=\\\"#{File.expand_path top_build_dir}/lib\\\""
    else
      spec.cc.flags << "-DMRBGEMS_ROOT=\"\"\\\"#{File.expand_path top_build_dir}/lib\\\"\"\""
    end
  end
end
