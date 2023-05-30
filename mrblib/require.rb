# Older than mruby-3.0.0, top-level local variables are destroyed,
# causing problems with `Kernel#require` and `Kernel#load`.
# To deal with this, wrap the whole thing in a proc and apply by `.call` immediately.
-> {
  loading_path = []
  runner = ->(lib, path) { instance_eval(&lib) }

  Kernel.define_method :require, &->(path) {
    lib, path = Kernel.__require_load_library(path, true, nil)
    if lib.kind_of?(Proc)
      return false if loading_path.include?(path)

      loading_path << path
      begin
        runner.call(lib, path)
        $" << path
        true
      ensure
        loading_path.pop rescue nil
      end
    else
      $" << path if lib
      lib # true or false
    end
  }

  Kernel.define_method :load, &->(path, wrap = false) {
    case
    when !wrap
      wrap = nil
    when wrap.kind_of?(Module) && !wrap.kind_of?(Class)
      # use module as is
    else
      wrap = Module.new
    end

    lib, path = Kernel.__require_load_library(path, false, wrap)
    runner.call(lib, nil)
    true
  }
}.call
