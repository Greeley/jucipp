#include "source.h"
#include "sourcefile.h"
#include <boost/property_tree/json_parser.hpp>
#include <fstream>
#include <boost/timer/timer.hpp>
#include "logging.h"
#include <algorithm>
#include <regex>
#include "selectiondialog.h"

bool Source::Config::legal_extension(std::string e) const {
  std::transform(e.begin(), e.end(),e.begin(), ::tolower);
  if (find(extensions.begin(), extensions.end(), e) != extensions.end()) {
    DEBUG("Legal extension");
    return true;
  }
  DEBUG("Ilegal extension");
  return false;
}

//////////////
//// View ////
//////////////
Source::View::View(const Source::Config& config, const std::string& file_path, const std::string& project_path):
config(config), file_path(file_path), project_path(project_path) {
  Gsv::init();
  set_smart_home_end(Gsv::SMART_HOME_END_BEFORE);
  set_show_line_numbers(config.show_line_numbers);
  set_highlight_current_line(config.highlight_current_line);
  sourcefile s(file_path);
  get_source_buffer()->get_undo_manager()->begin_not_undoable_action();
  get_source_buffer()->set_text(s.get_content());
  get_source_buffer()->get_undo_manager()->end_not_undoable_action();
  search_start = search_end = this->get_buffer()->end();
}

string Source::View::get_line(size_t line_number) {
  Gtk::TextIter line_it = get_source_buffer()->get_iter_at_line(line_number);
  Gtk::TextIter line_end_it = line_it;
  while(!line_end_it.ends_line())
    line_end_it++;
  std::string line(get_source_buffer()->get_text(line_it, line_end_it));
  return line;
}

string Source::View::get_line_before_insert() {
  Gtk::TextIter insert_it = get_source_buffer()->get_insert()->get_iter();
  Gtk::TextIter line_it = get_source_buffer()->get_iter_at_line(insert_it.get_line());
  std::string line(get_source_buffer()->get_text(line_it, insert_it));
  return line;
}

//Basic indentation
bool Source::View::on_key_press(GdkEventKey* key) {
  const std::regex spaces_regex(std::string("^(")+config.tab_char+"*).*$");
  //Indent as in next or previous line
  if(key->keyval==GDK_KEY_Return && key->state==0) {
    int line_nr=get_source_buffer()->get_insert()->get_iter().get_line();
    string line(get_line_before_insert());
    std::smatch sm;
    if(std::regex_match(line, sm, spaces_regex)) {
      if((line_nr+1)<get_source_buffer()->get_line_count()) {
        string next_line=get_line(line_nr+1);
        std::smatch sm2;
        if(std::regex_match(next_line, sm2, spaces_regex)) {
          if(sm2[1].str().size()>sm[1].str().size()) {
            get_source_buffer()->insert_at_cursor("\n"+sm2[1].str());
            scroll_to(get_source_buffer()->get_insert());
            return true;
          }
        }
      }
      get_source_buffer()->insert_at_cursor("\n"+sm[1].str());
      scroll_to(get_source_buffer()->get_insert());
      return true;
    }
  }
  //Indent right when clicking tab, no matter where in the line the cursor is. Also works on selected text.
  else if(key->keyval==GDK_KEY_Tab && key->state==0) {
    Gtk::TextIter selection_start, selection_end;
    get_source_buffer()->get_selection_bounds(selection_start, selection_end);
    int line_start=selection_start.get_line();
    int line_end=selection_end.get_line();
    for(int line=line_start;line<=line_end;line++) {
      Gtk::TextIter line_it = get_source_buffer()->get_iter_at_line(line);
      get_source_buffer()->insert(line_it, config.tab);
    }
    return true;
  }
  //Indent left when clicking shift-tab, no matter where in the line the cursor is. Also works on selected text.
  else if((key->keyval==GDK_KEY_ISO_Left_Tab || key->keyval==GDK_KEY_Tab) && key->state==GDK_SHIFT_MASK) {
    Gtk::TextIter selection_start, selection_end;
    get_source_buffer()->get_selection_bounds(selection_start, selection_end);
    int line_start=selection_start.get_line();
    int line_end=selection_end.get_line();
    
    for(int line_nr=line_start;line_nr<=line_end;line_nr++) {
      string line=get_line(line_nr);
      if(!(line.size()>=config.tab_size && line.substr(0, config.tab_size)==config.tab))
        return true;
    }
    
    for(int line_nr=line_start;line_nr<=line_end;line_nr++) {
      Gtk::TextIter line_it = get_source_buffer()->get_iter_at_line(line_nr);
      Gtk::TextIter line_plus_it=line_it;
      
      for(unsigned c=0;c<config.tab_size;c++)
        line_plus_it++;
      get_source_buffer()->erase(line_it, line_plus_it);
    }
    return true;
  }
  //"Smart" backspace key
  else if(key->keyval==GDK_KEY_BackSpace) {
    Gtk::TextIter insert_it=get_source_buffer()->get_insert()->get_iter();
    int line_nr=insert_it.get_line();
    if(line_nr>0) {
      string line=get_line(line_nr);
      string previous_line=get_line(line_nr-1);
      smatch sm;
      if(std::regex_match(previous_line, sm, spaces_regex)) {
        if(line==sm[1] || line==(std::string(sm[1])+config.tab) || (line+config.tab==sm[1])) {
          Gtk::TextIter line_it = get_source_buffer()->get_iter_at_line(line_nr);
          get_source_buffer()->erase(line_it, insert_it);
        }
      }
    }
  }
  return false;
}

//////////////////
//// ClangView ///
//////////////////
clang::Index Source::ClangView::clang_index(0, 0);

Source::ClangView::ClangView(const Source::Config& config, const std::string& file_path, const std::string& project_path, Terminal::Controller& terminal):
Source::View(config, file_path, project_path), terminal(terminal),
parse_thread_go(true), parse_thread_mapped(false), parse_thread_stop(false) {
  override_font(Pango::FontDescription(config.font));
  override_background_color(Gdk::RGBA(config.background));
  for (auto &item : config.tags) {
    get_source_buffer()->create_tag(item.first)->property_foreground() = item.second;
  }
  
  int start_offset = get_source_buffer()->begin().get_offset();
  int end_offset = get_source_buffer()->end().get_offset();
  auto buffer_map=get_buffer_map();
  //Remove includes for first parse for initial syntax highlighting
  auto& str=buffer_map[file_path];
  std::size_t pos=0;
  while((pos=str.find("#include", pos))!=std::string::npos) {
    auto start_pos=pos;
    pos=str.find('\n', pos+8);
    if(pos==std::string::npos)
      break;
    if(start_pos==0 || str[start_pos-1]=='\n') {
      str.replace(start_pos, pos-start_pos, pos-start_pos, ' ');
    }
    pos++;
  }
  init_syntax_highlighting(buffer_map,
                           start_offset,
                           end_offset,
                           &ClangView::clang_index);
  update_syntax(extract_tokens(0, get_source_buffer()->get_text().size())); //TODO: replace get_source_buffer()->get_text().size()
  
  //GTK-calls must happen in main thread, so the parse_thread
  //sends signals to the main thread that it is to call the following functions:
  parse_start.connect([this]{
    if(parse_thread_buffer_map_mutex.try_lock()) {
      parse_thread_buffer_map=get_buffer_map();
      parse_thread_mapped=true;
      parse_thread_buffer_map_mutex.unlock();
    }
    parse_thread_go=true;
  });
  
  parsing_in_progress=this->terminal.print_in_progress("Parsing "+file_path);
  parse_done.connect([this](){
    if(parse_thread_mapped) {
      INFO("Updating syntax");
      update_syntax(extract_tokens(0, get_source_buffer()->get_text().size()));
      parsing_in_progress->done("done");
      INFO("Syntax updated");
      update_diagnostics();
    }
    else {
      parse_thread_go=true;
    }
  });
  
  parse_thread=std::thread([this]() {
    while(true) {
      while(!parse_thread_go && !parse_thread_stop)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      if(parse_thread_stop)
        break;
      if(!parse_thread_mapped) {
        parse_thread_go=false;
        parse_start();
      }
      else if (parse_thread_mapped && parsing_mutex.try_lock() && parse_thread_buffer_map_mutex.try_lock()) {
        reparse(parse_thread_buffer_map);
        parse_thread_go=false;
        parsing_mutex.unlock();
        parse_thread_buffer_map_mutex.unlock();
        parse_done();
      }
    }
  });
  
  get_source_buffer()->signal_changed().connect([this]() {
    parse_thread_mapped=false;
    parse_thread_go=true;
  });
  
  signal_key_press_event().connect(sigc::mem_fun(*this, &Source::ClangView::on_key_press), false);
  signal_key_release_event().connect(sigc::mem_fun(*this, &Source::ClangView::on_key_release), false);
  signal_motion_notify_event().connect(sigc::mem_fun(*this, &Source::ClangView::clangview_on_motion_notify_event), false);
  get_buffer()->signal_mark_set().connect(sigc::mem_fun(*this, &Source::ClangView::clangview_on_mark_set), false);
}

Source::ClangView::~ClangView() {
  //TODO: Is it possible to stop the clang-process in progress?
  parsing_in_progress->cancel("canceled");
  parse_thread_stop=true;
  if(parse_thread.joinable())
    parse_thread.join();
  parsing_mutex.lock(); //Be sure not to destroy while still parsing with libclang
  parsing_mutex.unlock();
}

void Source::ClangView::
init_syntax_highlighting(const std::map<std::string, std::string>
                         &buffers,
                         int start_offset,
                         int end_offset,
                         clang::Index *index) {
  std::vector<string> arguments = get_compilation_commands();
  tu_ = std::unique_ptr<clang::TranslationUnit>(new clang::TranslationUnit(index,
                                                                           file_path,
                                                                           arguments,
                                                                           buffers));
}

std::map<std::string, std::string> Source::ClangView::
get_buffer_map() const {
  std::map<std::string, std::string> buffer_map;
  buffer_map[file_path]=get_source_buffer()->get_text().raw();
  return buffer_map;
}

int Source::ClangView::
reparse(const std::map<std::string, std::string> &buffer) {
  return tu_->ReparseTranslationUnit(file_path, buffer);
}

std::vector<Source::AutoCompleteData> Source::ClangView::
get_autocomplete_suggestions(int line_number, int column) {
  INFO("Getting auto complete suggestions");
  std::vector<Source::AutoCompleteData> suggestions;
  std::map<std::string, std::string> buffer_map;
  buffer_map[file_path]=get_source_buffer()->get_text(get_source_buffer()->begin(), get_source_buffer()->get_insert()->get_iter());
  buffer_map[file_path]+="\n";
  parsing_mutex.lock();
  clang::CodeCompleteResults results(tu_.get(),
                                     file_path,
                                     buffer_map,
                                     line_number,
                                     column-1);
  for (int i = 0; i < results.size(); i++) {
    const vector<clang::CompletionChunk> chunks_ = results.get(i).get_chunks();
    std::vector<AutoCompleteChunk> chunks;
    for (auto &chunk : chunks_) {
      chunks.emplace_back(chunk);
    }
    suggestions.emplace_back(chunks);
  }
  parsing_mutex.unlock();
  DEBUG("Number of suggestions");
  DEBUG_VAR(suggestions.size());
  return suggestions;
}

std::vector<std::string> Source::ClangView::
get_compilation_commands() {
  clang::CompilationDatabase db(project_path);
  clang::CompileCommands commands(file_path, &db);
  std::vector<clang::CompileCommand> cmds = commands.get_commands();
  std::vector<std::string> arguments;
  for (auto &i : cmds) {
    std::vector<std::string> lol = i.get_command_as_args();
    for (size_t a = 1; a < lol.size()-4; a++) {
      arguments.emplace_back(lol[a]);
    }
  }
  if(boost::filesystem::path(file_path).extension()==".h") //TODO: temporary fix for .h-files (parse as c++)
    arguments.emplace_back("-xc++");
  return arguments;
}

std::vector<Source::Range> Source::ClangView::
extract_tokens(int start_offset, int end_offset) {
  std::vector<Source::Range> ranges;
  clang::SourceLocation start(tu_.get(), file_path, start_offset);
  clang::SourceLocation end(tu_.get(), file_path, end_offset);
  clang::SourceRange range(&start, &end);
  clang::Tokens tokens(tu_.get(), &range);
  tokens.get_token_types(tu_.get());
  std::vector<clang::Token>& tks = tokens.tokens();
  update_types(tks);
  for (auto &token : tks) {
    switch (token.kind()) {
    case 0: highlight_cursor(&token, &ranges); break;  // PunctuationToken
    case 1: highlight_token(&token, &ranges, 702); break;  // KeywordToken
    case 2: highlight_cursor(&token, &ranges); break;  // IdentifierToken
    case 3: highlight_token(&token, &ranges, 109); break;  // LiteralToken
    case 4: highlight_token(&token, &ranges, 705); break;  // CommentToken
    }
  }
  return ranges;
}

void Source::ClangView::update_syntax(const std::vector<Source::Range> &ranges) {
  if (ranges.empty() || ranges.size() == 0) {
    return;
  }
  auto buffer = get_source_buffer();
  buffer->remove_all_tags(buffer->begin(), buffer->end());
  for (auto &range : ranges) {
    std::string type = std::to_string(range.kind);
    try {
      config.types.at(type);
    } catch (std::exception) {
      continue;
    }
    int linum_start = range.start.line_number-1;
    int linum_end = range.end.line_number-1;
    int begin = range.start.column_offset-1;
    int end = range.end.column_offset-1;
    
    if (end < 0) end = 0;
    if (begin < 0) begin = 0;
    Gtk::TextIter begin_iter =
    buffer->get_iter_at_line_offset(linum_start, begin);
    Gtk::TextIter end_iter  =
    buffer->get_iter_at_line_offset(linum_end, end);
    buffer->apply_tag_by_name(config.types.at(type),
                              begin_iter, end_iter);
  }
}

void Source::ClangView::update_diagnostics() {
  diagnostic_tooltips.clear();
  auto diagnostics=tu_->get_diagnostics();
  for(auto& diagnostic: diagnostics) {
    if(diagnostic.path==file_path) {
      auto start=get_buffer()->get_iter_at_offset(diagnostic.start_location.offset);
      auto end=get_buffer()->get_iter_at_offset(diagnostic.end_location.offset);
      std::string diagnostic_tag_name;
      if(diagnostic.severity<=CXDiagnostic_Warning)
        diagnostic_tag_name="diagnostic_warning";
      else
        diagnostic_tag_name="diagnostic_error";
      
      std::string spelling=diagnostic.spelling;
      std::string severity_spelling=diagnostic.severity_spelling;
      auto get_tooltip_buffer=[this, spelling, severity_spelling, diagnostic_tag_name]() {
        auto tooltip_buffer=Gtk::TextBuffer::create(get_buffer()->get_tag_table());
        tooltip_buffer->insert_with_tag(tooltip_buffer->get_insert()->get_iter(), severity_spelling, diagnostic_tag_name);
        tooltip_buffer->insert_at_cursor(":\n"+spelling);
        //TODO: Insert newlines to diagnostic.spelling (use 80 chars, then newline?)
        return tooltip_buffer;
      };
      diagnostic_tooltips.emplace_back(get_tooltip_buffer, *this, get_buffer()->create_mark(start), get_buffer()->create_mark(end));
      
      auto tag=get_buffer()->create_tag();
      tag->property_underline()=Pango::Underline::UNDERLINE_ERROR;
      auto tag_class=G_OBJECT_GET_CLASS(tag->gobj()); //For older GTK+ 3 versions:
      auto param_spec=g_object_class_find_property(tag_class, "underline-rgba");
      if(param_spec!=NULL) {
        auto diagnostic_tag=get_buffer()->get_tag_table()->lookup(diagnostic_tag_name);
        if(diagnostic_tag!=0)
          tag->set_property("underline-rgba", diagnostic_tag->property_foreground_rgba().get_value());
      }
      get_buffer()->apply_tag(tag, start, end);
    }
  }
}

void Source::ClangView::update_types(std::vector<clang::Token>& tokens) {
  type_tooltips.clear();
  for(auto& token: tokens) {
    if(token.type!="") {
      clang::SourceRange range(tu_.get(), &token);
      clang::SourceLocation start(&range, true);
      clang::SourceLocation end(&range, false);
      std::string path;
      unsigned start_offset, end_offset;
      start.get_location_info(&path, NULL, NULL, &start_offset);
      end.get_location_info(NULL, NULL, NULL, &end_offset);
      if(path==file_path) {
        auto start=get_buffer()->get_iter_at_offset(start_offset);
        auto end=get_buffer()->get_iter_at_offset(end_offset);
        auto get_tooltip_buffer=[this, token]() {
          auto tooltip_buffer=Gtk::TextBuffer::create(get_buffer()->get_tag_table());
          tooltip_buffer->insert_at_cursor("Type: "+token.type);
          return tooltip_buffer;
        };
        
        type_tooltips.emplace_back(get_tooltip_buffer, *this, get_buffer()->create_mark(start), get_buffer()->create_mark(end));
      }
    }
  }
}

bool Source::ClangView::clangview_on_motion_notify_event(GdkEventMotion* event) {
  Gdk::Rectangle rectangle(event->x, event->y, 1, 1);
  diagnostic_tooltips.init();
  type_tooltips.show(rectangle);
  diagnostic_tooltips.show(rectangle);
  return false;
}

void Source::ClangView::clangview_on_mark_set(const Gtk::TextBuffer::iterator& iterator, const Glib::RefPtr<Gtk::TextBuffer::Mark>& mark) {
  if(mark->get_name()=="insert") {
    Gdk::Rectangle rectangle;
    get_iter_location(iterator, rectangle);
    int location_window_x, location_window_y;
    buffer_to_window_coords(Gtk::TextWindowType::TEXT_WINDOW_TEXT, rectangle.get_x(), rectangle.get_y(), location_window_x, location_window_y);
    rectangle.set_x(location_window_x-2);
    rectangle.set_y(location_window_y);
    rectangle.set_width(4);
    diagnostic_tooltips.init();
    type_tooltips.show(rectangle);
    diagnostic_tooltips.show(rectangle);
  }
}

void Source::ClangView::
highlight_cursor(clang::Token *token,
                std::vector<Source::Range> *source_ranges) {
  clang::SourceLocation location = token->get_source_location(tu_.get());
  clang::Cursor cursor(tu_.get(), &location);
  clang::SourceRange range(&cursor);
  clang::SourceLocation begin(&range, true);
  clang::SourceLocation end(&range, false);
  unsigned begin_line_num, begin_offset, end_line_num, end_offset;
  begin.get_location_info(NULL, &begin_line_num, &begin_offset, NULL);
  end.get_location_info(NULL, &end_line_num, &end_offset, NULL);
  source_ranges->emplace_back(Source::Location(begin_line_num,
                                               begin_offset),
                              Source::Location(end_line_num,
                                               end_offset), (int) cursor.kind());
}
void Source::ClangView::
highlight_token(clang::Token *token,
               std::vector<Source::Range> *source_ranges,
               int token_kind) {
  clang::SourceRange range = token->get_source_range(tu_.get());
  unsigned begin_line_num, begin_offset, end_line_num, end_offset;
  clang::SourceLocation begin(&range, true);
  clang::SourceLocation end(&range, false);
  begin.get_location_info(NULL, &begin_line_num, &begin_offset, NULL);
  end.get_location_info(NULL, &end_line_num, &end_offset, NULL);
  source_ranges->emplace_back(Source::Location(begin_line_num,
                                               begin_offset),
                              Source::Location(end_line_num,
                                               end_offset), token_kind);
}

bool Source::ClangView::on_key_release(GdkEventKey* key) {
  INFO("Source::ClangView::on_key_release getting iters");
  //  Get function to fill popup with suggests item vector under is for testing
  Gtk::TextIter beg = get_source_buffer()->get_insert()->get_iter();
  Gtk::TextIter end = get_source_buffer()->get_insert()->get_iter();
  Gtk::TextIter tmp = get_source_buffer()->get_insert()->get_iter();
  Gtk::TextIter tmp1 = get_source_buffer()->get_insert()->get_iter();
  Gtk::TextIter line = get_source_buffer()->get_iter_at_line(tmp.get_line());
  if (end.backward_char() && end.backward_char()) {
    bool illegal_chars =
    end.backward_search("\"", Gtk::TEXT_SEARCH_VISIBLE_ONLY, tmp, tmp1, line)
    ||
      end.backward_search("//", Gtk::TEXT_SEARCH_VISIBLE_ONLY, tmp, tmp1, line);
    INFO("Source::ClangView::on_key_release checking key->keyval");
      if (illegal_chars) {
        return false;
      }
      std::string c = get_source_buffer()->get_text(end, beg);
      switch (key->keyval) {
      case 46:
        break;
      case 58:
        if (c != "::") return false;
        break;
      case 60:
        if (c != "->") return false;
        break;
      case 62:
        if (c != "->") return false;
        break;
      default:
        return false;
      }
  } else {
    return false;
  }
  INFO("Source::ClangView::on_key_release getting autocompletions");
  std::vector<Source::AutoCompleteData> acdata=get_autocomplete_suggestions(beg.get_line()+1,
                                                                            beg.get_line_offset()+2);
  std::map<std::string, std::string> rows;
  for (auto &data : acdata) {
    std::stringstream ss;
    std::string return_value;
    for (auto &chunk : data.chunks) {
      switch (chunk.kind) {
      case clang::CompletionChunk_ResultType:
        return_value = chunk.chunk;
        break;
      case clang::CompletionChunk_Informative: break;
      default: ss << chunk.chunk; break;
      }
    }
    if (ss.str().length() > 0) { // if length is 0 the result is empty
      rows[ss.str() + " --> " + return_value] = ss.str();
    }
  }
  if (rows.empty()) {
    rows["No suggestions found..."] = "";
  }
  
  SelectionDialog selection_dialog(*this);
  selection_dialog.on_select=[this, &rows](Gtk::ListViewText& list_view_text){
    std::string selected = rows.at(list_view_text.get_text(list_view_text.get_selected()[0]));
    get_source_buffer()->insert_at_cursor(selected);
  };
  selection_dialog.show(rows);
  
  return true;
}

//Clang indentation
//TODO: replace indentation methods with a better implementation or maybe use libclang
bool Source::ClangView::on_key_press(GdkEventKey* key) {
  const std::regex bracket_regex(std::string("^(")+config.tab_char+"*).*\\{ *$");
  const std::regex no_bracket_statement_regex(std::string("^(")+config.tab_char+"*)(if|for|else if|catch|while) *\\(.*[^;}] *$");
  const std::regex no_bracket_no_para_statement_regex(std::string("^(")+config.tab_char+"*)(else|try|do) *$");
  const std::regex spaces_regex(std::string("^(")+config.tab_char+"*).*$");
  
  //Indent depending on if/else/etc and brackets
  if(key->keyval==GDK_KEY_Return && key->state==0) {
    string line(get_line_before_insert());
    std::smatch sm;
    if(std::regex_match(line, sm, bracket_regex)) {
      int line_nr=get_source_buffer()->get_insert()->get_iter().get_line();
      if((line_nr+1)<get_source_buffer()->get_line_count()) {
        string next_line=get_line(line_nr+1);
        std::smatch sm2;
        if(std::regex_match(next_line, sm2, spaces_regex)) {
          if(sm2[1].str()==sm[1].str()+config.tab) {
            get_source_buffer()->insert_at_cursor("\n"+sm[1].str()+config.tab);
            scroll_to(get_source_buffer()->get_insert());
            return true;
          }
        }
      }
      get_source_buffer()->insert_at_cursor("\n"+sm[1].str()+config.tab+"\n"+sm[1].str()+"}");
      auto insert_it = get_source_buffer()->get_insert()->get_iter();
      for(size_t c=0;c<config.tab_size+sm[1].str().size();c++)
        insert_it--;
      scroll_to(get_source_buffer()->get_insert());
      get_source_buffer()->place_cursor(insert_it);
      return true;
    }
    else if(std::regex_match(line, sm, no_bracket_statement_regex)) {
      get_source_buffer()->insert_at_cursor("\n"+sm[1].str()+config.tab);
      scroll_to(get_source_buffer()->get_insert());
      return true;
    }
    else if(std::regex_match(line, sm, no_bracket_no_para_statement_regex)) {
      get_source_buffer()->insert_at_cursor("\n"+sm[1].str()+config.tab);
      scroll_to(get_source_buffer()->get_insert());
      return true;
    }
    else if(std::regex_match(line, sm, spaces_regex)) {
      std::smatch sm2;
      size_t line_nr=get_source_buffer()->get_insert()->get_iter().get_line();
      if(line_nr>0 && sm[1].str().size()>=config.tab_size) {
        string previous_line=get_line(line_nr-1);
        if(!std::regex_match(previous_line, sm2, bracket_regex)) {
          if(std::regex_match(previous_line, sm2, no_bracket_statement_regex)) {
            get_source_buffer()->insert_at_cursor("\n"+sm2[1].str());
            scroll_to(get_source_buffer()->get_insert());
            return true;
          }
          else if(std::regex_match(previous_line, sm2, no_bracket_no_para_statement_regex)) {
            get_source_buffer()->insert_at_cursor("\n"+sm2[1].str());
            scroll_to(get_source_buffer()->get_insert());
            return true;
          }
        }
      }
    }
  }
  //Indent left when writing } on a new line
  else if(key->keyval==GDK_KEY_braceright) {
    string line=get_line_before_insert();
    if(line.size()>=config.tab_size) {
      for(auto c: line) {
        if(c!=config.tab_char)
          return false;
      }
      Gtk::TextIter insert_it = get_source_buffer()->get_insert()->get_iter();
      Gtk::TextIter line_it = get_source_buffer()->get_iter_at_line(insert_it.get_line());
      Gtk::TextIter line_plus_it=line_it;
      for(unsigned c=0;c<config.tab_size;c++)
        line_plus_it++;
      
      get_source_buffer()->erase(line_it, line_plus_it);
    }
    return false;
  }
  
  return Source::View::on_key_press(key);
}

////////////////////
//// Controller ////
////////////////////

// Source::Controller::Controller()
// Constructor for Controller
Source::Controller::Controller(const Source::Config &config,
                               const std::string& file_path, std::string project_path, Terminal::Controller& terminal) {
  if(project_path=="") {
    project_path=boost::filesystem::path(file_path).parent_path().string();
  }
  if (config.legal_extension(file_path.substr(file_path.find_last_of(".") + 1)))
    view=std::unique_ptr<View>(new ClangView(config, file_path, project_path, terminal));
  else
    view=std::unique_ptr<View>(new GenericView(config, file_path, project_path));
  INFO("Source Controller with childs constructed");
}

Glib::RefPtr<Gsv::Buffer> Source::Controller::buffer() {
  return view->get_source_buffer();
}
