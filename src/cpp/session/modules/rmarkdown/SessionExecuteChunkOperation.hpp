/*
 * SessionExecuteChunkOperation.hpp
 *
 * Copyright (C) 2009-16 by RStudio, Inc.
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */
#ifndef SESSION_MODULES_RMARKDOWN_SESSION_EXECUTE_CHUNK_OPERATIONR_HPP
#define SESSION_MODULES_RMARKDOWN_SESSION_EXECUTE_CHUNK_OPERATIONR_HPP

#include <boost/bind.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/noncopyable.hpp>
#include <boost/shared_ptr.hpp>

#include <core/FileSerializer.hpp>
#include <core/text/CsvParser.hpp>
#include <core/system/Process.hpp>
#include <core/system/ShellUtils.hpp>
#include <core/system/System.hpp>

#include "NotebookOutput.hpp"
#include "NotebookExec.hpp"
#include "SessionRmdNotebook.hpp"

namespace rstudio {
namespace session {
namespace modules {
namespace rmarkdown {
namespace notebook {

core::shell_utils::ShellCommand shellCommandForEngine(
      const std::string& engine,
      const core::FilePath& scriptPath)
{
   core::shell_utils::ShellCommand command(engine);
   
   if (engine == "Rscript")
      command << "-f";
   
   command << scriptPath;
   
   return command;
}

class ExecuteChunkOperation : boost::noncopyable,
                              public boost::enable_shared_from_this<ExecuteChunkOperation>
{
   typedef core::shell_utils::ShellCommand ShellCommand;
   typedef core::system::ProcessCallbacks ProcessCallbacks;
   typedef core::system::ProcessOperations ProcessOperations;
   
public:
   static boost::shared_ptr<ExecuteChunkOperation> create(const std::string& docId,
                                                          const std::string& chunkId,
                                                          const ShellCommand& command)
   {
      boost::shared_ptr<ExecuteChunkOperation> pProcess =
            boost::shared_ptr<ExecuteChunkOperation>(new ExecuteChunkOperation(
                                                        docId,
                                                        chunkId,
                                                        command));
      pProcess->registerProcess();
      return pProcess;
   }
   
private:
   
   ExecuteChunkOperation(const std::string& docId,
                         const std::string& chunkId,
                         const ShellCommand& command)
      : terminationRequested_(false),
        docId_(docId),
        chunkId_(chunkId),
        command_(command)
   {
      using namespace core;
      Error error = Success();
      
      // ensure regular directory
      FilePath outputPath = chunkOutputPath(
               docId_,
               chunkId_,
               ContextExact);
      
      error = outputPath.removeIfExists();
      if (error)
         LOG_ERROR(error);
      
      error = outputPath.ensureDirectory();
      if (error)
         LOG_ERROR(error);
      
      // clean old chunk output
      error = cleanChunkOutput(docId_, chunkId_, true);
      if (error)
         LOG_ERROR(error);
   }
   
public:
   
   ProcessCallbacks processCallbacks()
   {
      ProcessCallbacks callbacks;
      
      callbacks.onStarted = boost::bind(
               &ExecuteChunkOperation::onStarted,
               shared_from_this());
      
      callbacks.onContinue = boost::bind(
               &ExecuteChunkOperation::onContinue,
               shared_from_this());
      
      callbacks.onStdout = boost::bind(
               &ExecuteChunkOperation::onStdout,
               shared_from_this(),
               _2);
      
      callbacks.onStderr = boost::bind(
               &ExecuteChunkOperation::onStderr,
               shared_from_this(),
               _2);
      
      callbacks.onExit = boost::bind(
               &ExecuteChunkOperation::onExit,
               shared_from_this(),
               _1);
      
      return callbacks;
   }
   
private:
   
   enum OutputType { OUTPUT_STDOUT, OUTPUT_STDERR };
   
   void onStarted()
   {
   }
   
   bool onContinue()
   {
      return !terminationRequested_;
   }
   
   void onExit(int exitStatus)
   {
      events().onChunkExecCompleted(docId_, chunkId_, notebookCtxId());
      deregisterProcess();
   }
   
   void onStdout(const std::string& output)
   {
      onText(output, OUTPUT_STDOUT);
   }
   
   void onStderr(const std::string& output)
   {
      onText(output, OUTPUT_STDERR);
   }
   
   void onText(const std::string& output, OutputType outputType)
   {
      using namespace core;
      
      // get path to cache file
      FilePath target = chunkOutputFile(docId_, chunkId_, kChunkOutputText);
      
      // generate CSV to write
      std::vector<std::string> values;
      values.push_back(outputType == OUTPUT_STDOUT
               ? safe_convert::numberToString(kChunkConsoleOutput)
               : safe_convert::numberToString(kChunkConsoleError));
      values.push_back(output);
      std::string encoded = text::encodeCsvLine(values) + "\n";
      
      // write to cache
      Error error = writeStringToFile(target, encoded);
      if (error)
      {
         LOG_ERROR(error);
         return;
      }
      
      // emit client event
      enqueueChunkOutput(
               docId_,
               chunkId_,
               notebookCtxId(),
               kChunkOutputText,
               target);
   }
   
public:
   
   void terminate() { terminationRequested_ = true; }
   bool terminationRequested() const { return terminationRequested_; }
   const std::string& chunkId() const { return chunkId_; }
   
private:
   bool terminationRequested_;
   std::string docId_;
   std::string chunkId_;
   ShellCommand command_;
   
private:
   
   typedef std::map<
      std::string,
      boost::shared_ptr<ExecuteChunkOperation>
   > ProcessRegistry;
   
   static ProcessRegistry& registry()
   {
      static ProcessRegistry instance;
      return instance;
   }
   
   void registerProcess()
   {
      registry()[docId_ + "-" + chunkId_] = shared_from_this();
   }
   
   void deregisterProcess()
   {
      registry().erase(docId_ + "-" + chunkId_);
   }
   
public:
   static boost::shared_ptr<ExecuteChunkOperation> getProcess(
         const std::string& docId,
         const std::string& chunkId)
   {
      return registry()[docId + "-" + chunkId];
   }
};

core::Error runChunk(const std::string& docId,
                     const std::string& chunkId,
                     const std::string& engine,
                     const std::string& code)
{
   using namespace core;
   typedef core::shell_utils::ShellCommand ShellCommand;
   
   // write code to temporary file
   FilePath scriptPath = module_context::tempFile("chunk-code", "");
   Error error = core::writeStringToFile(scriptPath, code);
   if (error)
   {
      LOG_ERROR(error);
      return error;
   }
   
   // get command
   ShellCommand command = notebook::shellCommandForEngine(engine, scriptPath);

   // create and run process
   boost::shared_ptr<ExecuteChunkOperation> operation =
         ExecuteChunkOperation::create(docId, chunkId, command);

   // generate process options
   core::system::ProcessOptions options;
   options.terminateChildren = true;
   
   error = module_context::processSupervisor().runCommand(
            command,
            options,
            operation->processCallbacks());
   
   if (error)
      LOG_ERROR(error);
   
   return Success();
}


} // end namespace notebook
} // end namespace rmarkdown
} // end namespace modules
} // end namespace session
} // end namespace rstudio

#endif /* SESSION_MODULES_RMARKDOWN_SESSION_EXECUTE_CHUNK_OPERATIONR_HPP */