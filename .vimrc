" Project-local C editing and Makefile commands.
set nocompatible
set encoding=utf-8
set expandtab
set shiftwidth=4
set tabstop=4
set autoindent
set number
set tags=./tags;,tags
set makeprg=make
nnoremap <leader>m :make<CR>
nnoremap <leader>t :make test<CR>
nnoremap <leader>f :make fmt<CR>
