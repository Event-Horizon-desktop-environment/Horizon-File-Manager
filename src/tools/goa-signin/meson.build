# horizon-goa-signin: genuine GOA add-dialog host (GTK4 + libgoa-backend).
# Optional: skipped silently when the stack is absent.
dep_gtk4_si = dependency('gtk4', required: false)
dep_adw_si = dependency('libadwaita-1', required: false)
dep_goa_si = dependency('goa-1.0', required: false)
dep_goa_backend_si = dependency('goa-backend-1.0', required: false)

if dep_gtk4_si.found() and dep_adw_si.found() and dep_goa_si.found() and dep_goa_backend_si.found()
  executable('horizon-goa-signin',
    'goa-signin.c',
    c_args: ['-DGOA_API_IS_SUBJECT_TO_CHANGE', '-DGOA_BACKEND_API_IS_SUBJECT_TO_CHANGE'],
    dependencies: [dep_gtk4_si, dep_adw_si, dep_goa_si, dep_goa_backend_si],
    install: true,
  )
else
  message('horizon-goa-signin: skipped (need gtk4 + libadwaita-1 + goa-1.0 + goa-backend-1.0)')
endif
