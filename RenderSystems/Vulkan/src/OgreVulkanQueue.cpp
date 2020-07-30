/*
-----------------------------------------------------------------------------
This source file is part of OGRE
    (Object-oriented Graphics Rendering Engine)
For the latest info, see http://www.ogre3d.org/

Copyright (c) 2000-present Torus Knot Software Ltd

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------
*/

#include "OgreVulkanQueue.h"

#include "OgreVulkanMappings.h"
#include "OgreVulkanRenderSystem.h"
#include "OgreVulkanTextureGpu.h"
#include "OgreVulkanWindow.h"
#include "Vao/OgreVulkanVaoManager.h"

#include "OgreException.h"
#include "OgrePixelFormatGpuUtils.h"
#include "OgreStringConverter.h"

#include "OgreVulkanUtils.h"

#include <vulkan/vulkan.h>

#define TODO_findRealPresentQueue
#define TODO_we_assume_has_stencil
#define TODO__notifyActiveEncoderEnded
#define TODO_endEncodersAreNeverCalled

namespace Ogre
{
    VulkanQueue::VulkanQueue() :
        mDevice( 0 ),
        mFamily( NumQueueFamilies ),
        mFamilyIdx( 0u ),
        mQueueIdx( 0u ),
        mQueue( 0 ),
        mCurrentCmdBuffer( 0 ),
        mOwnerDevice( 0 ),
        mVaoManager( 0 ),
        mRenderSystem( 0 ),
        mCurrentFence( 0 ),
        mEncoderState( EncoderClosed ),
        mCopyEndReadSrcBufferFlags( 0 ),
        mCopyEndReadDstBufferFlags( 0 ),
        mCopyEndReadDstTextureFlags( 0 ),
        mCopyStartWriteSrcBufferFlags( 0 )
    {
    }
    //-------------------------------------------------------------------------
    VulkanQueue::~VulkanQueue()
    {
        if( mDevice )
        {
            vkDeviceWaitIdle( mDevice );

            {
                FastArray<PerFrameData>::iterator itor = mPerFrameData.begin();
                FastArray<PerFrameData>::iterator endt = mPerFrameData.end();

                while( itor != endt )
                {
                    VkFenceArray::const_iterator itFence = itor->mProtectingFences.begin();
                    VkFenceArray::const_iterator enFence = itor->mProtectingFences.end();

                    while( itFence != enFence )
                        vkDestroyFence( mDevice, *itFence++, 0 );

                    itor->mProtectingFences.clear();
                }
            }

            VkFenceArray::const_iterator itor = mAvailableFences.begin();
            VkFenceArray::const_iterator endt = mAvailableFences.end();

            while( itor != endt )
                vkDestroyFence( mDevice, *itor++, 0 );

            mAvailableFences.clear();
        }
    }
    //-------------------------------------------------------------------------
    VkFence VulkanQueue::getFence( void )
    {
        VkFence retVal;
        if( !mAvailableFences.empty() )
        {
            retVal = mAvailableFences.back();
            mAvailableFences.pop_back();
        }
        else
        {
            VkFenceCreateInfo fenceCi;
            makeVkStruct( fenceCi, VK_STRUCTURE_TYPE_FENCE_CREATE_INFO );
            VkResult result = vkCreateFence( mDevice, &fenceCi, 0, &retVal );
            checkVkResult( result, "vkCreateFence" );
        }
        return retVal;
    }
    //-------------------------------------------------------------------------
    VkCommandBuffer VulkanQueue::getCmdBuffer( size_t currFrame )
    {
        PerFrameData &frameData = mPerFrameData[currFrame];

        if( frameData.mCurrentCmdIdx >= frameData.mCommands.size() )
        {
            VkCommandBuffer cmdBuffer;

            VkCommandBufferAllocateInfo allocateInfo;
            makeVkStruct( allocateInfo, VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO );
            allocateInfo.commandPool = frameData.mCmdPool;
            allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocateInfo.commandBufferCount = 1u;
            VkResult result = vkAllocateCommandBuffers( mDevice, &allocateInfo, &cmdBuffer );
            checkVkResult( result, "vkAllocateCommandBuffers" );

            frameData.mCommands.push_back( cmdBuffer );
        }
        else if( frameData.mCurrentCmdIdx == 0u )
            vkResetCommandPool( mDevice, frameData.mCmdPool, 0 );

        return frameData.mCommands[frameData.mCurrentCmdIdx++];
    }
    //-------------------------------------------------------------------------
    void VulkanQueue::setQueueData( VulkanDevice *owner, QueueFamily family, uint32 familyIdx,
                                    uint32 queueIdx )
    {
        mOwnerDevice = owner;
        mFamily = family;
        mFamilyIdx = familyIdx;
        mQueueIdx = queueIdx;
    }
    //-------------------------------------------------------------------------
    void VulkanQueue::init( VkDevice device, VkQueue queue, VulkanRenderSystem *renderSystem )
    {
        mDevice = device;
        mQueue = queue;
        mVaoManager = static_cast<VulkanVaoManager *>( renderSystem->getVaoManager() );
        mRenderSystem = renderSystem;

        const size_t maxNumFrames = mVaoManager->getDynamicBufferMultiplier();

        VkDeviceQueueCreateInfo queueCreateInfo;
        makeVkStruct( queueCreateInfo, VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO );

        // Create at least maxNumFrames fences, though we may need more
        mAvailableFences.resize( maxNumFrames );

        VkFenceCreateInfo fenceCi;
        makeVkStruct( fenceCi, VK_STRUCTURE_TYPE_FENCE_CREATE_INFO );
        for( size_t i = 0; i < maxNumFrames; ++i )
        {
            VkResult result = vkCreateFence( mDevice, &fenceCi, 0, &mAvailableFences[i] );
            checkVkResult( result, "vkCreateFence" );
        }

        // Create one cmd pool per thread (assume single threaded for now)
        mPerFrameData.resize( maxNumFrames );
        VkCommandPoolCreateInfo cmdPoolCreateInfo;
        makeVkStruct( cmdPoolCreateInfo, VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO );
        cmdPoolCreateInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        cmdPoolCreateInfo.queueFamilyIndex = mFamilyIdx;

        for( size_t i = 0; i < maxNumFrames; ++i )
            vkCreateCommandPool( mDevice, &cmdPoolCreateInfo, 0, &mPerFrameData[i].mCmdPool );

        newCommandBuffer();
    }

    void VulkanQueue::destroy() {}

    //-------------------------------------------------------------------------
    void VulkanQueue::newCommandBuffer( void )
    {
        const size_t currFrame = mVaoManager->waitForTailFrameToFinish();
        mCurrentCmdBuffer = getCmdBuffer( currFrame );

        VkCommandBufferBeginInfo beginInfo;
        makeVkStruct( beginInfo, VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO );
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer( mCurrentCmdBuffer, &beginInfo );
    }
    //-------------------------------------------------------------------------
    void VulkanQueue::endCommandBuffer( void )
    {
        if( mCurrentCmdBuffer )
        {
            endAllEncoders();

            VkResult result = vkEndCommandBuffer( mCurrentCmdBuffer );
            checkVkResult( result, "vkEndCommandBuffer" );

            mPendingCmds.push_back( mCurrentCmdBuffer );
            mCurrentCmdBuffer = 0;
        }
    }
    //-------------------------------------------------------------------------
    void VulkanQueue::getGraphicsEncoder( void )
    {
        if( mEncoderState != EncoderGraphicsOpen )
        {
            endCopyEncoder();
            endComputeEncoder();

            mEncoderState = EncoderGraphicsOpen;
        }
    }
    //-------------------------------------------------------------------------
    void VulkanQueue::getComputeEncoder( void )
    {
        if( mEncoderState != EncoderComputeOpen )
        {
            endRenderEncoder();
            endCopyEncoder();

            mEncoderState = EncoderComputeOpen;
        }
    }
    //-------------------------------------------------------------------------
    VkPipelineStageFlags VulkanQueue::deriveStageFromBufferAccessFlags( VkAccessFlags accessFlags )
    {
        VkPipelineStageFlags stage = 0;
        if( accessFlags & VK_ACCESS_INDIRECT_COMMAND_READ_BIT )
            stage |= VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
        if( accessFlags & ( VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT ) )
        {
            stage |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        }
        if( accessFlags &
            ( VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT ) )
        {
            stage |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                     VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
                     VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT |
                     VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        }
        if( accessFlags & ( VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT ) )
        {
            stage |= VK_PIPELINE_STAGE_TRANSFER_BIT;
        }

        return stage;
    }
    //-------------------------------------------------------------------------
    VkPipelineStageFlags VulkanQueue::deriveStageFromTextureAccessFlags( VkAccessFlags accessFlags )
    {
        VkPipelineStageFlags stage = 0;
        if( accessFlags & ( VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT ) )
        {
            stage |=
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        }
        if( accessFlags &
            ( VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT ) )
        {
            stage |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        }
        if( accessFlags & ( VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT ) )
        {
            stage |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                     VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
                     VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT |
                     VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        }
        if( accessFlags & ( VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT ) )
        {
            stage |= VK_PIPELINE_STAGE_TRANSFER_BIT;
        }

        return stage;
    }
    //-------------------------------------------------------------------------
    void VulkanQueue::insertRestoreBarrier( VulkanTextureGpu *vkTexture,
                                            const VkImageLayout newTransferLayout )
    {
        const VkImageLayout oldLayout = vkTexture->mCurrLayout;

        const VkImageLayout otherTransferLayout =
            newTransferLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

        const VkAccessFlags accessFlags = newTransferLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                              ? VK_ACCESS_TRANSFER_READ_BIT
                                              : VK_ACCESS_TRANSFER_WRITE_BIT;

        if( oldLayout == newTransferLayout )
        {
            // Nothing to do. A restore barrier has already been inserted
            // If the assert fails, then the texture transitioned
            // to this layout without us knowing
            OGRE_ASSERT_HIGH( std::find( mImageMemBarrierPtrs.begin(), mImageMemBarrierPtrs.end(),
                                         vkTexture ) != mImageMemBarrierPtrs.end() &&
                              "Only this class should set VK_IMAGE_LAYOUT_TRANSFER_*_OPTIMAL" );
        }
        else if( oldLayout == otherTransferLayout )
        {
            // A restore barrier has already been inserted, but it needs modification
            FastArray<TextureGpu *>::iterator itor =
                std::find( mImageMemBarrierPtrs.begin(), mImageMemBarrierPtrs.end(), vkTexture );

            // If the assert fails, then the texture transitioned
            // to this layout without us knowing
            OGRE_ASSERT_LOW( itor != mImageMemBarrierPtrs.end() &&
                             "Only this class should set VK_IMAGE_LAYOUT_TRANSFER_*_OPTIMAL" );

            const size_t idx = ( size_t )( itor - mImageMemBarrierPtrs.begin() );
            VkImageMemoryBarrier &imageMemBarrier = *( mImageMemBarriers.begin() + idx );
            imageMemBarrier.srcAccessMask = accessFlags;
            imageMemBarrier.oldLayout = newTransferLayout;
        }
        else
        {
            // First time we see this texture
            VkImageMemoryBarrier imageMemBarrier = vkTexture->getImageMemoryBarrier();
            imageMemBarrier.srcAccessMask = accessFlags;
            imageMemBarrier.dstAccessMask = VulkanMappings::get( vkTexture );
            imageMemBarrier.oldLayout = newTransferLayout;
            imageMemBarrier.newLayout = vkTexture->mNextLayout;
            mImageMemBarriers.push_back( imageMemBarrier );
            mImageMemBarrierPtrs.push_back( vkTexture );
        }
    }
    //-------------------------------------------------------------------------
    void VulkanQueue::prepareForUpload( const BufferPacked *buffer, TextureGpu *texture )
    {
        VkAccessFlags bufferAccessFlags = 0;

        if( buffer )
        {
            BufferPackedDownloadMap::iterator it = mCopyDownloadBuffers.find( buffer );

            if( it == mCopyDownloadBuffers.end() )
                bufferAccessFlags = VulkanMappings::get( buffer->getBufferPackedType() );
            else
            {
                if( !it->second )
                {
                    // bufferAccessFlags = VK_ACCESS_TRANSFER_WRITE_BIT;
                    // We assume consecutive writes means we're writing to non-overlapping areas
                    // Do not wait for previous transfers.
                    bufferAccessFlags = 0;
                }
                else
                    bufferAccessFlags = VK_ACCESS_TRANSFER_READ_BIT;
            }

            mCopyDownloadBuffers[buffer] = false;

            mCopyEndReadSrcBufferFlags |= VK_ACCESS_TRANSFER_WRITE_BIT;
        }

        OGRE_ASSERT_HIGH( !texture || dynamic_cast<VulkanTextureGpu *>( texture ) );
        VulkanTextureGpu *vkTexture = static_cast<VulkanTextureGpu *>( texture );

        VkAccessFlags texAccessFlags = 0;

        if( texture )
        {
            TextureGpuDownloadMap::iterator it = mCopyDownloadTextures.find( vkTexture );

            if( vkTexture->mCurrLayout == VK_IMAGE_LAYOUT_UNDEFINED )
            {
                // This texture must just have been created
                texAccessFlags = 0;
            }
            else if( it == mCopyDownloadTextures.end() )
            {
                texAccessFlags = VulkanMappings::get( texture );
            }
            else
            {
                if( !it->second )
                {
                    OGRE_ASSERT_MEDIUM( vkTexture->mCurrLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );
                    // texAccessFlags = VK_ACCESS_TRANSFER_WRITE_BIT;
                    // We assume consecutive writes means we're writing to non-overlapping areas
                    // Do not wait for previous transfers.
                    texAccessFlags = 0;
                }
                else
                {
                    OGRE_ASSERT_MEDIUM( vkTexture->mCurrLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL );
                    texAccessFlags = VK_ACCESS_TRANSFER_READ_BIT;
                }
            }

            mCopyEndReadDstTextureFlags |= VulkanMappings::get( texture );
            mCopyDownloadTextures[vkTexture] = false;
        }

        // One buffer barrier is enough for all buffers.
        // Unless we already issued a transfer to this same buffer
        const bool bNeedsBufferBarrier =
            ( bufferAccessFlags &&
              ( mCopyEndReadDstBufferFlags & bufferAccessFlags ) != bufferAccessFlags ) ||
            ( bufferAccessFlags & ( VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT ) );

        const bool bNeedsTexTransition =
            vkTexture && vkTexture->mCurrLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

        mCopyEndReadDstBufferFlags |= bufferAccessFlags;

        // Trigger the barrier if we actually have to wait.
        // And only if we haven't issued this barrier already
        if( bNeedsBufferBarrier || bNeedsTexTransition )
        {
            VkPipelineStageFlags srcStage = 0;

            uint32 numMemBarriers = 0u;
            VkMemoryBarrier memBarrier;
            if( bNeedsBufferBarrier )
            {
                // GPU must stop using this buffer before we can write into it
                makeVkStruct( memBarrier, VK_STRUCTURE_TYPE_MEMORY_BARRIER );
                memBarrier.srcAccessMask = bufferAccessFlags;
                memBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

                srcStage |= deriveStageFromBufferAccessFlags( bufferAccessFlags );
                numMemBarriers = 1u;
            }

            uint32 numImageMemBarriers = 0u;
            VkImageMemoryBarrier imageMemBarrier;
            if( bNeedsTexTransition )
            {
                // GPU must stop using this texture before we can write into it
                // Also we need to do a transition
                imageMemBarrier = vkTexture->getImageMemoryBarrier();
                imageMemBarrier.srcAccessMask = texAccessFlags;
                imageMemBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                imageMemBarrier.oldLayout = vkTexture->mCurrLayout;
                imageMemBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

                if( texAccessFlags == 0u )
                {
                    if( bufferAccessFlags == 0u )
                    {
                        // Wait for nothing. We're only issuing a barrier
                        // because of the texture transition
                        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
                    }
                }
                else
                {
                    srcStage |= deriveStageFromTextureAccessFlags( texAccessFlags );
                }

                insertRestoreBarrier( vkTexture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );

                vkTexture->mCurrLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                numImageMemBarriers = 1u;
            }

            // Wait until earlier render, compute and transfers are done so we can copy what
            // they wrote (unless we're only here for a texture transition)
            vkCmdPipelineBarrier( mCurrentCmdBuffer, srcStage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                  numMemBarriers, &memBarrier, 0u, 0, numImageMemBarriers,
                                  &imageMemBarrier );
        }
    }
    //-------------------------------------------------------------------------
    void VulkanQueue::prepareForDownload( const BufferPacked *buffer, TextureGpu *texture )
    {
        VkAccessFlags bufferAccessFlags = 0;
        VkPipelineStageFlags srcStage = 0;

        // Evaluate the stages which blocks us before we can begin our transfer
        if( buffer )
        {
            BufferPackedDownloadMap::iterator it = mCopyDownloadBuffers.find( buffer );

            if( it == mCopyDownloadBuffers.end() )
            {
                if( buffer->getBufferPackedType() == BP_TYPE_UAV )
                {
                    bufferAccessFlags = VK_ACCESS_SHADER_WRITE_BIT;
                    srcStage |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
                                VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT |
                                VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT |
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
                }
                // else
                //{
                // If the buffer is not BT_TYPE_UAV, the GPU won't modify these buffers,
                // we can start downloading right away without waiting
                //}
            }
            else
            {
                if( !it->second )
                    bufferAccessFlags = VK_ACCESS_TRANSFER_WRITE_BIT;
                else
                    bufferAccessFlags = 0;  // Consecutive reads don't require waiting
            }

            mCopyDownloadBuffers[buffer] = true;

            mCopyEndReadSrcBufferFlags |= VK_ACCESS_TRANSFER_READ_BIT;
        }

        OGRE_ASSERT_HIGH( !texture || dynamic_cast<VulkanTextureGpu *>( texture ) );
        VulkanTextureGpu *vkTexture = static_cast<VulkanTextureGpu *>( texture );

        VkAccessFlags texAccessFlags = 0;

        if( texture )
        {
            TextureGpuDownloadMap::iterator it = mCopyDownloadTextures.find( vkTexture );

            if( it == mCopyDownloadTextures.end() )
            {
                if( texture->isUav() )
                {
                    texAccessFlags |= VK_ACCESS_SHADER_WRITE_BIT;
                    srcStage |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
                                VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT |
                                VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT |
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
                }

                if( texture->isRenderToTexture() )
                {
                    texAccessFlags |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                    if( !PixelFormatGpuUtils::isDepth( texture->getPixelFormat() ) )
                    {
                        texAccessFlags |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                        srcStage |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                    }
                    else
                    {
                        texAccessFlags |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                        srcStage |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
                    }
                }
            }
            else
            {
                if( !it->second )
                {
                    OGRE_ASSERT_MEDIUM( vkTexture->mCurrLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL );
                    texAccessFlags = VK_ACCESS_TRANSFER_WRITE_BIT;
                }
                else
                {
                    OGRE_ASSERT_MEDIUM( vkTexture->mCurrLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL );
                    texAccessFlags = 0;  // Consecutive reads don't require waiting
                }
            }

            mCopyDownloadTextures[vkTexture] = true;
        }

        // One buffer barrier is enough for all buffers.
        // Unless we already issued a transfer to this same buffer
        const bool bNeedsBufferBarrier =
            ( bufferAccessFlags &&
              ( mCopyStartWriteSrcBufferFlags & bufferAccessFlags ) != bufferAccessFlags ) ||
            ( bufferAccessFlags & ( VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT ) );

        mCopyStartWriteSrcBufferFlags |= bufferAccessFlags;

        const bool bNeedsTexTransition =
            vkTexture && vkTexture->mCurrLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

        // Trigger the barrier if we actually have to wait.
        // And only if we haven't issued this barrier already
        if( bNeedsBufferBarrier || bNeedsTexTransition )
        {
            uint32 numMemBarriers = 0u;
            VkMemoryBarrier memBarrier;
            if( bNeedsBufferBarrier )
            {
                makeVkStruct( memBarrier, VK_STRUCTURE_TYPE_MEMORY_BARRIER );
                memBarrier.srcAccessMask = bufferAccessFlags;
                memBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                numMemBarriers = 1u;
            }

            uint32 numImageMemBarriers = 0u;
            VkImageMemoryBarrier imageMemBarrier;
            if( bNeedsTexTransition )
            {
                imageMemBarrier = vkTexture->getImageMemoryBarrier();
                imageMemBarrier.srcAccessMask = texAccessFlags;
                imageMemBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                imageMemBarrier.oldLayout = vkTexture->mCurrLayout;
                imageMemBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

                insertRestoreBarrier( vkTexture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL );

                vkTexture->mCurrLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                numImageMemBarriers = 1u;
            }

            // Wait until earlier render, compute and transfers are done so we can copy what
            // they wrote (unless we're only here for a texture transition)
            vkCmdPipelineBarrier( mCurrentCmdBuffer, srcStage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                  numMemBarriers, &memBarrier, 0u, 0, numImageMemBarriers,
                                  &imageMemBarrier );
        }
    }
    //-------------------------------------------------------------------------
    void VulkanQueue::getCopyEncoder( const BufferPacked *buffer, TextureGpu *texture,
                                      const bool bDownload )
    {
        if( mEncoderState != EncoderCopyOpen )
        {
            endRenderEncoder();
            endComputeEncoder();

            mEncoderState = EncoderCopyOpen;

            // Submission guarantees the host write being complete, as per
            // khronos.org/registry/vulkan/specs/1.0/html/vkspec.html#synchronization-submission-host-writes
            // So no need for a barrier before the transfer
            //
            // The only exception is when writing from CPU to GPU when a command that uses that region
            // has already been submitted via vkQueueSubmit (and you're using vkCmdWaitEvents to wait
            // for the CPU to write the data and give ok to the GPU).
            // Which Ogre does not do (too complex to get right).
        }

        OGRE_ASSERT_MEDIUM( ( buffer || texture ) &&
                            "Buffer or Texture at least one must be a valid ptr" );

        if( bDownload )
            prepareForDownload( buffer, texture );
        else
            prepareForUpload( buffer, texture );
    }
    //-------------------------------------------------------------------------
    void VulkanQueue::getCopyEncoderV1Buffer( const bool bDownload )
    {
        if( mEncoderState != EncoderCopyOpen )
        {
            endRenderEncoder();
            endComputeEncoder();

            mEncoderState = EncoderCopyOpen;

            if( !bDownload )
            {
                // V1 buffers are only used for vertex and index buffers
                // We assume v1 buffers don't try to write then read (or read then write) in a row
                const VkAccessFlags bufferAccessFlags =
                    VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;

                if( ( mCopyEndReadDstBufferFlags & bufferAccessFlags ) != bufferAccessFlags )
                {
                    uint32 numMemBarriers = 0u;
                    VkMemoryBarrier memBarrier;
                    makeVkStruct( memBarrier, VK_STRUCTURE_TYPE_MEMORY_BARRIER );
                    memBarrier.srcAccessMask = bufferAccessFlags;
                    memBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                    numMemBarriers = 1u;

                    // GPU must stop using this buffer before we can write into it
                    vkCmdPipelineBarrier( mCurrentCmdBuffer, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1u, &memBarrier, 0u, 0, 0u,
                                          0 );
                }

                mCopyEndReadDstBufferFlags |= bufferAccessFlags;
                mCopyEndReadSrcBufferFlags |= VK_ACCESS_TRANSFER_WRITE_BIT;
            }
            else
            {
                mCopyEndReadSrcBufferFlags |= VK_ACCESS_TRANSFER_READ_BIT;
            }
        }
    }
    //-------------------------------------------------------------------------
    void VulkanQueue::endCopyEncoder( void )
    {
        if( mEncoderState != EncoderCopyOpen )
            return;

        if( mCopyEndReadDstBufferFlags || !mImageMemBarrierPtrs.empty() )
        {
            VkPipelineStageFlags dstStage = 0;

            uint32 numMemBarriers = 0u;
            VkMemoryBarrier memBarrier;
            if( mCopyEndReadDstBufferFlags )
            {
                makeVkStruct( memBarrier, VK_STRUCTURE_TYPE_MEMORY_BARRIER );
                memBarrier.srcAccessMask = mCopyEndReadSrcBufferFlags;
                memBarrier.dstAccessMask = mCopyEndReadDstBufferFlags;

                // Evaluate the stages we can unblock when our transfers are done
                dstStage |= deriveStageFromBufferAccessFlags( memBarrier.dstAccessMask );
                numMemBarriers = 1u;
            }

            dstStage |= deriveStageFromTextureAccessFlags( mCopyEndReadDstTextureFlags );

            // Wait until earlier render, compute and transfers are done
            // Block render, compute and transfers until we're done
            vkCmdPipelineBarrier( mCurrentCmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, dstStage, 0,
                                  numMemBarriers, &memBarrier, 0u, 0,
                                  static_cast<uint32_t>( mImageMemBarriers.size() ),
                                  mImageMemBarriers.begin() );

            mImageMemBarriers.clear();
            mImageMemBarrierPtrs.clear();

            TextureGpuDownloadMap::const_iterator itor = mCopyDownloadTextures.begin();
            TextureGpuDownloadMap::const_iterator endt = mCopyDownloadTextures.end();

            while( itor != endt )
            {
                itor->first->mCurrLayout = itor->first->mNextLayout;
                ++itor;
            }
        }

        mCopyEndReadSrcBufferFlags = 0;
        mCopyEndReadDstBufferFlags = 0;
        mCopyEndReadDstTextureFlags = 0;
        mCopyStartWriteSrcBufferFlags = 0;

        mCopyDownloadTextures.clear();
        mCopyDownloadBuffers.clear();

        mEncoderState = EncoderClosed;
    }
    //-------------------------------------------------------------------------
    void VulkanQueue::endRenderEncoder( const bool endRenderPassDesc )
    {
        if( mEncoderState != EncoderGraphicsOpen )
            return;
        mEncoderState = EncoderClosed;

        TODO__notifyActiveEncoderEnded;
        // mRenderSystem->_notifyActiveEncoderEnded( endRenderPassDesc );
    }
    //-------------------------------------------------------------------------
    void VulkanQueue::endComputeEncoder( void )
    {
        if( mEncoderState != EncoderComputeOpen )
            return;
        mEncoderState = EncoderClosed;
    }
    //-------------------------------------------------------------------------
    void VulkanQueue::endAllEncoders( const bool endRenderPassDesc )
    {
        endCopyEncoder();
        endRenderEncoder( endRenderPassDesc );
        endComputeEncoder();
    }
    //-------------------------------------------------------------------------
    void VulkanQueue::addWindowToWaitFor( VkSemaphore imageAcquisitionSemaph )
    {
        OGRE_ASSERT_MEDIUM( mFamily == Graphics );
        mGpuWaitFlags.push_back( VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT );
        mGpuWaitSemaphForCurrCmdBuff.push_back( imageAcquisitionSemaph );
    }
    //-------------------------------------------------------------------------
    void VulkanQueue::_waitOnFrame( uint8 frameIdx )
    {
        FastArray<VkFence> &fences = mPerFrameData[frameIdx].mProtectingFences;

        if( !fences.empty() )
        {
            const uint32 numFences = static_cast<uint32>( fences.size() );
            vkWaitForFences( mDevice, numFences, &fences[0], VK_TRUE, UINT64_MAX );
            vkResetFences( mDevice, numFences, &fences[0] );
            mAvailableFences.appendPOD( fences.begin(), fences.end() );
            fences.clear();
        }
    }
    //-------------------------------------------------------------------------
    bool VulkanQueue::_isFrameFinished( uint8 frameIdx )
    {
        bool bIsFinished = true;
        FastArray<VkFence> &fences = mPerFrameData[frameIdx].mProtectingFences;

        if( !fences.empty() )
        {
            const uint32 numFences = static_cast<uint32>( fences.size() );
            VkResult result = vkWaitForFences( mDevice, numFences, &fences[0], VK_TRUE, 0u );
            if( result != VK_TIMEOUT )
            {
                vkResetFences( mDevice, numFences, &fences[0] );
                mAvailableFences.appendPOD( fences.begin(), fences.end() );
                fences.clear();
            }
            else
            {
                bIsFinished = false;
            }
        }

        return bIsFinished;
    }
    //-------------------------------------------------------------------------
    void VulkanQueue::commitAndNextCommandBuffer( bool endingFrame )
    {
        VkSubmitInfo submitInfo;
        makeVkStruct( submitInfo, VK_STRUCTURE_TYPE_SUBMIT_INFO );

        endCommandBuffer();

        if( mPendingCmds.empty() )
            return;

        const size_t windowsSemaphStart = mGpuSignalSemaphForCurrCmdBuff.size();
        const size_t numWindowsPendingSwap = mWindowsPendingSwap.size();
        mVaoManager->getAvailableSempaphores( mGpuSignalSemaphForCurrCmdBuff, numWindowsPendingSwap );

        if( !mGpuWaitSemaphForCurrCmdBuff.empty() )
        {
            submitInfo.waitSemaphoreCount = static_cast<uint32>( mGpuWaitSemaphForCurrCmdBuff.size() );
            submitInfo.pWaitSemaphores = mGpuWaitSemaphForCurrCmdBuff.begin();
            submitInfo.pWaitDstStageMask = mGpuWaitFlags.begin();
        }
        if( !mGpuSignalSemaphForCurrCmdBuff.empty() )
        {
            submitInfo.signalSemaphoreCount =
                static_cast<uint32>( mGpuSignalSemaphForCurrCmdBuff.size() );
            submitInfo.pSignalSemaphores = mGpuSignalSemaphForCurrCmdBuff.begin();
        }
        // clang-format off
        submitInfo.commandBufferCount   = static_cast<uint32>( mPendingCmds.size() );
        submitInfo.pCommandBuffers      = &mPendingCmds[0];
        // clang-format on

        const uint8 dynBufferFrame = mVaoManager->waitForTailFrameToFinish();
        VkFence fence = getCurrentFence();

        Log *defaultLog = LogManager::getSingleton().getDefaultLog();
        if( defaultLog )
        {
            defaultLog->logMessage( String( "VulkanQueue::commitAndNextCommandBuffer" ) );
        }

        vkQueueSubmit( mQueue, 1u, &submitInfo, fence );
        vkQueueWaitIdle( mQueue );

        mCurrentFence = 0;

        mPerFrameData[dynBufferFrame].mProtectingFences.push_back( fence );

        mPendingCmds.clear();

        for( size_t windowIdx = 0u; windowIdx < numWindowsPendingSwap; ++windowIdx )
        {
            VkSemaphore semaphore = mGpuSignalSemaphForCurrCmdBuff[windowsSemaphStart + windowIdx];
            mWindowsPendingSwap[windowIdx]->_swapBuffers( semaphore );
            mVaoManager->notifyWaitSemaphoreSubmitted( semaphore );
        }
        mWindowsPendingSwap.clear();

        mVaoManager->notifyWaitSemaphoresSubmitted( mGpuWaitSemaphForCurrCmdBuff );

        if( endingFrame )
        {
            mPerFrameData[dynBufferFrame].mCurrentCmdIdx = 0u;
            mVaoManager->_notifyNewCommandBuffer();
        }

        newCommandBuffer();

        mGpuWaitSemaphForCurrCmdBuff.clear();
        mGpuSignalSemaphForCurrCmdBuff.clear();
    }
}  // namespace Ogre
